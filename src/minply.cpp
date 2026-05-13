/*
 * minply - Minimal Audio Player
 *
 * Lightweight and fast audio player with BLE receiver lag compensation
 *
 * Usage:
 *   minply.exe [audio file path | -]
 *
 * Features:
 *   - Instantly plays MP3, WAV, AAC, FLAC, Opus and other audio files
 *   - Accepts audio data from stdin (no argument or - as argument)
 *   - Plays inaudible 19kHz guard tone before/after audio (BLE anti-clipping)
 *   - Exits immediately after playback completes
 *
 * Dependencies:
 *   - Windows Media Foundation: Audio decoder
 *   - libopus: Opus audio codec decoder
 *   - libogg: Ogg container format parser
 *   - WASAPI: Windows audio output
 *
 * Build:
 *   Visual Studio 2019 or later, Release configuration, x64
 */

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <vector>
#include <ogg/ogg.h>
#include <opus/opus.h>
#include <ebur128.h>
#include <cmath>
#include <string>
#include <cstdlib>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
// Note: opus.lib and ogg.lib are linked via build.ps1

// Error codes
#define EXIT_SUCCESS          0
#define ERR_INVALID_ARGS      1
#define ERR_FILE_NOT_FOUND    2
#define ERR_DECODE_FAILED     3
#define ERR_WASAPI_INIT       4
#define ERR_PLAYBACK_FAILED   5

// Constants
constexpr float LEAD_IN_DURATION = 1.2f;    // Lead-in duration in seconds; BLE wake-up (~700ms) + WASAPI session startup noise margin
constexpr float LEAD_OUT_DURATION = 1.2f;   // Lead-out duration in seconds; keep BLE active until audio tail drains through SBC codec pipeline
constexpr float BLE_GUARD_FREQ = 19000.0f;  // Guard tone frequency in Hz (inaudible to adults)
constexpr float BLE_GUARD_AMP = 0.001f;     // Guard tone amplitude (~-60dB)
constexpr float TWO_PI = 6.2831853f;        // 2π

constexpr float LOUDNESS_TARGET = -16.0f;        // Target integrated loudness in LUFS (optimized for notification sounds)
constexpr float LOUDNESS_PEAK_CEILING = 0.891f;  // True peak ceiling (-1dBFS); prevents clipping after loudness gain
constexpr float LOUDNESS_MIN_PEAK = 1e-6f;       // Minimum peak threshold; skip normalization for near-silence

constexpr float FADE_DURATION = 0.005f;    // Fade in/out duration in seconds (click noise reduction)
constexpr DWORD BUFFER_WAIT_MS = 100;      // Buffer wait time in milliseconds
constexpr DWORD DRAIN_WAIT_MS = 300;       // Wait time for device buffer drain in milliseconds
constexpr DWORD DRAIN_POLL_MS = 10;        // Polling interval while draining the WASAPI buffer
constexpr DWORD DRAIN_TIMEOUT_MS = 5000;   // Hard cap on drain polling to prevent infinite loops on stuck devices
constexpr int   RENDER_MAX_STALL_ITERATIONS = 100;  // ~10s of consecutive WAIT_TIMEOUT wakeups (100 * BUFFER_WAIT_MS) before aborting

// PCM integer-to-float scale factors (2^(bits-1))
constexpr float PCM16_SCALE = 32768.0f;        // 2^15
constexpr float PCM24_SCALE = 8388608.0f;      // 2^23
constexpr float PCM32_SCALE = 2147483648.0f;   // 2^31

// Opus decoder parameters
constexpr int  OPUS_OUTPUT_RATE     = 48000;   // Opus always decodes at 48kHz internally
constexpr int  OPUS_MAX_FRAME_SIZE  = 5760;    // 120ms at 48kHz; the largest frame opus_decode_float emits
constexpr int  OPUS_MAX_CHANNELS    = 8;       // Upper bound used for Opus pcmBuffer sizing

// WAV decoder parameters
constexpr WORD WAV_MAX_CHANNELS     = 8;       // WAVEFORMATEX channel upper bound accepted by this decoder

// I/O chunk sizes
constexpr size_t STDIN_INITIAL_RESERVE = 1024 * 1024;  // Pre-reserve to avoid reallocation for typical notification sounds
constexpr size_t STDIN_READ_CHUNK      = 65536;
constexpr size_t OGG_FEED_CHUNK        = 65536;

// Application configuration
//
// Loaded from minply.toml / minply.local.toml in the executable directory.
// Missing file or missing key falls back to the default value.
struct AppConfig {
    bool  guardEnabled        = true;
    float guardFrequency      = BLE_GUARD_FREQ;
    float guardAmplitude      = BLE_GUARD_AMP;
    float leadInDuration      = LEAD_IN_DURATION;
    float leadOutDuration     = LEAD_OUT_DURATION;
    bool  loudnessEnabled     = true;
    float loudnessTarget      = LOUDNESS_TARGET;
    float loudnessPeakCeiling = LOUDNESS_PEAK_CEILING;
};

// Print error message to stderr
void PrintError(const char* message) {
    std::cerr << "Error: " << message << std::endl;
}

// Get device mix format from WASAPI
bool GetDeviceMixFormat(WAVEFORMATEX** mixFormat) {
    HRESULT hr;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    bool success = false;

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        if (FAILED(hr)) break;

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) break;

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (FAILED(hr)) break;

        hr = client->GetMixFormat(mixFormat);
        if (SUCCEEDED(hr)) {
            success = true;
        }
    } while (false);

    if (client) client->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();

    return success;
}

// Forward declaration
std::vector<float> ConvertFormat(const std::vector<float>& input,
                                 UINT32 srcRate, UINT32 srcChannels,
                                 UINT32 dstRate, UINT32 dstChannels);

// Read WAV data from buffer (bypass MF resampling for matching formats)
bool TryReadWavBuffer(const BYTE* data, size_t size, std::vector<float>& audioData,
                     UINT32 targetSampleRate, UINT32 targetChannels) {
    size_t pos = 0;

    // バッファから n バイトを dst にコピーして pos を進める
    auto readBytes = [&](void* dst, size_t n) -> bool {
        if (n > size - pos) return false;
        memcpy(dst, data + pos, n);
        pos += n;
        return true;
    };
    // バッファ上で n バイト前進する（読み捨て）
    auto skipBytes = [&](size_t n) -> bool {
        if (n > size - pos) return false;
        pos += n;
        return true;
    };

    char header[12];
    if (!readBytes(header, 12)) return false;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) return false;

    WAVEFORMATEXTENSIBLE fmt = {};
    bool fmtFound = false;
    while (true) {
        char chunkId[4];
        DWORD chunkSize;
        if (!readBytes(chunkId, 4) || !readBytes(&chunkSize, 4)) break;

        if (memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) break;
            BYTE fmtBuf[40] = {};
            DWORD fmtReadSize = (std::min)(chunkSize, static_cast<DWORD>(40));
            if (!readBytes(fmtBuf, fmtReadSize)) break;
            memcpy(&fmt, fmtBuf, fmtReadSize);
            fmtFound = true;
            if (chunkSize > fmtReadSize && !skipBytes(chunkSize - fmtReadSize)) break;
            break;
        }
        if (!skipBytes(chunkSize)) break;
    }
    if (!fmtFound) return false;

    // Normalize WAVE_FORMAT_EXTENSIBLE to its underlying SubFormat tag so PCM/Float can share branches below
    WORD actualFormatTag = fmt.Format.wFormatTag;
    if (actualFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt.Format.cbSize >= 22) {
        actualFormatTag = *reinterpret_cast<const WORD*>(&fmt.SubFormat);
    }

    if (actualFormatTag != WAVE_FORMAT_PCM && actualFormatTag != WAVE_FORMAT_IEEE_FLOAT) return false;
    // Reject malformed headers that would later trigger divide-by-zero or oversized allocations
    if (fmt.Format.nChannels == 0 || fmt.Format.nChannels > WAV_MAX_CHANNELS) return false;
    if (fmt.Format.nSamplesPerSec == 0) return false;
    if (fmt.Format.wBitsPerSample == 0 || (fmt.Format.wBitsPerSample % 8) != 0) return false;
    if (fmt.Format.nSamplesPerSec != targetSampleRate) return false;

    // Find data chunk (rescan from offset 12)
    pos = 12;
    DWORD dataSize = 0;
    bool dataFound = false;
    while (true) {
        char chunkId[4];
        DWORD chunkSize;
        if (!readBytes(chunkId, 4) || !readBytes(&chunkSize, 4)) break;

        if (memcmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            dataFound = true;
            break;
        }
        if (!skipBytes(chunkSize)) break;
    }
    if (!dataFound || dataSize == 0) return false;
    if (dataSize > size - pos) return false;

    UINT32 bytesPerSample = fmt.Format.wBitsPerSample / 8;
    UINT32 totalSamples = dataSize / bytesPerSample;
    audioData.resize(totalSamples);

    const BYTE* rawData = data + pos;

    if (actualFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt.Format.wBitsPerSample == 32) {
        memcpy(audioData.data(), rawData, dataSize);
    }
    else if (actualFormatTag == WAVE_FORMAT_PCM) {
        if (fmt.Format.wBitsPerSample == 16) {
            const int16_t* samples = reinterpret_cast<const int16_t*>(rawData);
            for (UINT32 i = 0; i < totalSamples; i++) {
                audioData[i] = static_cast<float>(samples[i]) / PCM16_SCALE;
            }
        }
        else if (fmt.Format.wBitsPerSample == 24) {
            // Build via uint32_t to keep the left-shifts well-defined, then arithmetic-shift back to sign-extend.
            // (Shifting a signed int into the sign bit is UB even though MSVC tolerates it.)
            for (UINT32 i = 0; i < totalSamples; i++) {
                uint32_t u =  static_cast<uint32_t>(rawData[i * 3])      << 8
                           | static_cast<uint32_t>(rawData[i * 3 + 1]) << 16
                           | static_cast<uint32_t>(rawData[i * 3 + 2]) << 24;
                int32_t sample = static_cast<int32_t>(u) >> 8;
                audioData[i] = static_cast<float>(sample) / PCM24_SCALE;
            }
        }
        else if (fmt.Format.wBitsPerSample == 32) {
            const int32_t* samples = reinterpret_cast<const int32_t*>(rawData);
            for (UINT32 i = 0; i < totalSamples; i++) {
                audioData[i] = static_cast<float>(samples[i]) / PCM32_SCALE;
            }
        }
        else {
            return false;
        }
    }
    else {
        return false;
    }

    // Channel conversion (e.g. mono -> stereo)
    if (fmt.Format.nChannels != targetChannels) {
        audioData = ConvertFormat(audioData, targetSampleRate, fmt.Format.nChannels,
                                  targetSampleRate, targetChannels);
    }

    return true;
}

// Convert audio format (resampling and channel conversion)
std::vector<float> ConvertFormat(const std::vector<float>& input,
                                 UINT32 srcRate, UINT32 srcChannels,
                                 UINT32 dstRate, UINT32 dstChannels) {
    if (input.empty() || srcRate == 0 || dstRate == 0 || srcChannels == 0 || dstChannels == 0) {
        return {};
    }
    if (srcRate == dstRate && srcChannels == dstChannels) {
        return input;
    }

    size_t srcFrames = input.size() / srcChannels;
    if (srcFrames == 0) return {};
    size_t dstFrames = static_cast<size_t>((static_cast<uint64_t>(srcFrames) * dstRate) / srcRate);
    std::vector<float> output(dstFrames * dstChannels);

    for (size_t i = 0; i < dstFrames; i++) {
        float srcIndex = static_cast<float>(i * srcRate) / dstRate;
        size_t idx0 = static_cast<size_t>(srcIndex);
        size_t idx1 = (std::min)(idx0 + 1, srcFrames - 1);
        float frac = srcIndex - idx0;

        for (UINT32 ch = 0; ch < dstChannels; ch++) {
            UINT32 srcCh = (std::min)(ch, srcChannels - 1);
            float s0 = input[idx0 * srcChannels + srcCh];
            float s1 = input[idx1 * srcChannels + srcCh];
            output[i * dstChannels + ch] = s0 + (s1 - s0) * frac;
        }
    }

    return output;
}

// Decode Opus/Ogg data from buffer (.opus and .ogg Opus)
bool TryDecodeOpusBuffer(const BYTE* data, size_t size, std::vector<float>& audioData,
                         UINT32 targetSampleRate, UINT32 targetChannels) {
    ogg_sync_state   oy;
    ogg_stream_state os;
    ogg_page         og;
    ogg_packet       op;

    ogg_sync_init(&oy);
    bool streamInitialized = false;
    OpusDecoder* decoder = nullptr;
    int opusChannels = 0;
    std::vector<float> decodedFloat;
    // Heap-allocated PCM scratch buffer; stack allocation would consume ~180KB and risk overflow on deep call stacks
    std::vector<float> pcmBuffer(static_cast<size_t>(OPUS_MAX_FRAME_SIZE) * OPUS_MAX_CHANNELS);
    int packetCount = 0;

    // Feed entire buffer in chunks; ogg_sync_buffer reallocs are expensive for large single allocations
    size_t offset = 0;
    while (offset < size) {
        size_t toWrite = (std::min)(OGG_FEED_CHUNK, size - offset);
        char* buf = ogg_sync_buffer(&oy, static_cast<long>(toWrite));
        if (!buf) break;
        memcpy(buf, data + offset, toWrite);
        ogg_sync_wrote(&oy, static_cast<long>(toWrite));
        offset += toWrite;
    }

    // Opus stream structure: packet 1 = OpusHead, packet 2 = OpusTags, packet 3+ = audio data
    while (ogg_sync_pageout(&oy, &og) == 1) {
        if (!streamInitialized) {
            if (ogg_stream_init(&os, ogg_page_serialno(&og)) != 0) {
                ogg_sync_clear(&oy);
                return false;
            }
            streamInitialized = true;
        }
        ogg_stream_pagein(&os, &og);

        while (ogg_stream_packetout(&os, &op) == 1) {
            packetCount++;

            if (packetCount == 1) {
                // OpusHead packet carries channel count and reserves the codec context for audio packets
                if (op.bytes >= 19 && memcmp(op.packet, "OpusHead", 8) == 0) {
                    opusChannels = op.packet[9];
                    // Channel mapping family != 0 needs opus_multistream_decoder; reject and limit family 0 to mono/stereo per RFC 7845
                    int channelMappingFamily = op.packet[18];
                    if (channelMappingFamily != 0 || opusChannels < 1 || opusChannels > 2) {
                        if (streamInitialized) ogg_stream_clear(&os);
                        ogg_sync_clear(&oy);
                        return false;
                    }
                    int error;
                    decoder = opus_decoder_create(OPUS_OUTPUT_RATE, opusChannels, &error);
                    if (error != OPUS_OK || !decoder) {
                        if (streamInitialized) ogg_stream_clear(&os);
                        ogg_sync_clear(&oy);
                        return false;
                    }
                }
                else {
                    if (decoder) opus_decoder_destroy(decoder);
                    if (streamInitialized) ogg_stream_clear(&os);
                    ogg_sync_clear(&oy);
                    return false;
                }
            }
            else if (packetCount == 2) {
                // OpusTags (metadata) - intentionally skipped
                continue;
            }
            else {
                if (decoder) {
                    int frameSize = opus_decode_float(decoder, op.packet, op.bytes,
                                                      pcmBuffer.data(), OPUS_MAX_FRAME_SIZE, 0);
                    if (frameSize > 0) {
                        size_t sampleCount = static_cast<size_t>(frameSize) * opusChannels;
                        decodedFloat.insert(decodedFloat.end(),
                                            pcmBuffer.data(), pcmBuffer.data() + sampleCount);
                    }
                }
            }
        }
    }

    if (decoder) opus_decoder_destroy(decoder);
    if (streamInitialized) ogg_stream_clear(&os);
    ogg_sync_clear(&oy);

    if (decodedFloat.empty()) return false;

    audioData = ConvertFormat(decodedFloat, OPUS_OUTPUT_RATE, opusChannels,
                              targetSampleRate, targetChannels);

    return true;
}

// Decode audio data using Media Foundation
//
// Wraps the buffer as a seekable IStream (SHCreateMemStream) and feeds it to
// MFSourceReader. Seekability is required by most MF decoders (MP3, AAC, FLAC, etc.).
bool DecodeAudioBuffer(const BYTE* data, size_t size, std::vector<float>& decodedData,
                       UINT32 targetSampleRate, UINT32 targetChannels) {
    HRESULT hr;
    IStream* istream = nullptr;
    IMFByteStream* byteStream = nullptr;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* mediaType = nullptr;
    bool success = false;

    do {
        // SHCreateMemStream takes a UINT length; refuse to silently truncate >4GB inputs
        if (size > MAXUINT) {
            PrintError("Input too large for in-memory decode");
            break;
        }
        istream = SHCreateMemStream(data, static_cast<UINT>(size));
        if (!istream) {
            PrintError("Failed to create memory stream");
            break;
        }

        hr = MFCreateMFByteStreamOnStream(istream, &byteStream);
        if (FAILED(hr)) {
            PrintError("Failed to create MF byte stream");
            break;
        }

        hr = MFCreateSourceReaderFromByteStream(byteStream, nullptr, &reader);
        if (FAILED(hr)) {
            PrintError("Failed to open audio data");
            break;
        }

        // Output media type: 32-bit float PCM at the device's mix rate/channels
        hr = MFCreateMediaType(&mediaType);
        if (FAILED(hr)) break;

        hr = mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (FAILED(hr)) break;

        hr = mediaType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (FAILED(hr)) break;

        hr = mediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        if (FAILED(hr)) break;

        hr = mediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, targetSampleRate);
        if (FAILED(hr)) break;

        hr = mediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, targetChannels);
        if (FAILED(hr)) break;

        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mediaType);
        if (FAILED(hr)) {
            PrintError("Failed to set output format");
            break;
        }

        while (true) {
            DWORD flags = 0;
            IMFSample* sample = nullptr;

            hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                   nullptr, &flags, nullptr, &sample);
            if (FAILED(hr)) break;

            // MF reports stream-level errors via flags rather than HRESULT; honor them to avoid spinning forever
            if (flags & MF_SOURCE_READERF_ERROR) {
                if (sample) sample->Release();
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (sample) sample->Release();
                break;
            }
            // A mid-stream media-type change would force mixing samples of different formats; discard partial data and fail
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                if (sample) sample->Release();
                decodedData.clear();
                break;
            }

            if (sample) {
                IMFMediaBuffer* buffer = nullptr;
                hr = sample->ConvertToContiguousBuffer(&buffer);
                if (SUCCEEDED(hr)) {
                    BYTE* bufData = nullptr;
                    DWORD dataLen = 0;

                    hr = buffer->Lock(&bufData, nullptr, &dataLen);
                    if (SUCCEEDED(hr)) {
                        size_t sampleCount = dataLen / sizeof(float);
                        size_t oldSize = decodedData.size();
                        decodedData.resize(oldSize + sampleCount);
                        memcpy(&decodedData[oldSize], bufData, dataLen);
                        buffer->Unlock();
                    }
                    buffer->Release();
                }
                sample->Release();
            }
        }

        success = !decodedData.empty();

    } while (false);

    if (mediaType) mediaType->Release();
    if (reader) reader->Release();
    if (byteStream) byteStream->Release();
    if (istream) istream->Release();

    return success;
}

// Read all binary data from stdin into buffer
//
// Only reads when stdin is a pipe or redirected file to avoid blocking on
// interactive console input. Switches to binary mode to prevent CRLF translation.
// Returns false if stdin is not redirected, on read error, or no data was read.
bool ReadAllStdin(std::vector<BYTE>& buffer) {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (!hStdin || hStdin == INVALID_HANDLE_VALUE) return false;

    DWORD type = GetFileType(hStdin);
    if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK) return false;

    _setmode(_fileno(stdin), _O_BINARY);

    buffer.reserve(STDIN_INITIAL_RESERVE);

    BYTE chunk[STDIN_READ_CHUNK];
    while (true) {
        DWORD bytesRead = 0;
        // Distinguish EOF (success with 0 bytes) from read errors:
        // pipe disconnect / handle invalidation must not be treated as clean EOF
        // because that would silently truncate the input.
        if (!ReadFile(hStdin, chunk, sizeof(chunk), &bytesRead, nullptr)) {
            DWORD err = GetLastError();
            // ERROR_BROKEN_PIPE on a closed write-end is a normal end-of-stream
            if (err == ERROR_BROKEN_PIPE) break;
            return false;
        }
        if (bytesRead == 0) break;
        // Cap at 4GB; the in-memory decode path uses SHCreateMemStream (UINT length)
        if (bytesRead > MAXUINT - buffer.size()) {
            PrintError("stdin input exceeds 4GB limit");
            return false;
        }
        buffer.insert(buffer.end(), chunk, chunk + bytesRead);
    }
    return !buffer.empty();
}

// Generate inaudible sine wave buffer for BLE anti-clipping
// BLE devices enter power-saving mode on digital silence, causing audio clipping.
// A 19kHz tone at ~-60dB is inaudible to adults but keeps the BLE codec active.
std::vector<float> GenerateBleGuard(UINT32 sampleRate, UINT32 channels, float duration,
                                     float freq, float amp) {
    size_t totalFrames = static_cast<size_t>(sampleRate * duration);
    std::vector<float> buffer(totalFrames * channels);

    for (size_t i = 0; i < totalFrames; i++) {
        float t = static_cast<float>(i) / sampleRate;
        float sample = amp * sinf(TWO_PI * freq * t);
        for (UINT32 ch = 0; ch < channels; ch++) {
            buffer[i * channels + ch] = sample;
        }
    }

    return buffer;
}

// Normalize audio using EBU R128 integrated loudness (ITU-R BS.1770-4)
//
// Measures integrated loudness via libebur128, computes gain to reach target,
// then clamps gain if true peak would exceed peakCeiling.
void NormalizeLoudness(std::vector<float>& audioData, UINT32 sampleRate, UINT32 channels,
                       float target, float peakCeiling) {
    if (audioData.empty()) return;

    float peak = 0.0f;
    for (float s : audioData) {
        float v = fabsf(s);
        if (v > peak) peak = v;
    }
    if (peak < LOUDNESS_MIN_PEAK) return;

    ebur128_state* state = ebur128_init(channels, sampleRate, EBUR128_MODE_I);
    if (!state) return;

    size_t frames = audioData.size() / channels;
    if (ebur128_add_frames_float(state, audioData.data(), frames) != EBUR128_SUCCESS) {
        ebur128_destroy(&state);
        return;
    }

    double loudness = 0.0;
    int result = ebur128_loudness_global(state, &loudness);
    ebur128_destroy(&state);

    if (result != EBUR128_SUCCESS || !std::isfinite(loudness)) return;

    float gain = static_cast<float>(pow(10.0, (target - loudness) / 20.0));

    if (peak * gain > peakCeiling) {
        gain = peakCeiling / peak;
    }

    for (float& s : audioData) {
        s *= gain;
    }
}

// Apply fade-in/out to audio data to prevent click noise from waveform discontinuity
void ApplyFade(std::vector<float>& audioData, UINT32 sampleRate, UINT32 channels) {
    UINT32 fadeFrames = static_cast<UINT32>(sampleRate * FADE_DURATION);
    UINT32 totalFrames = static_cast<UINT32>(audioData.size() / channels);
    if (totalFrames < fadeFrames * 2) return; // too short for fade

    // Fade in
    for (UINT32 i = 0; i < fadeFrames; i++) {
        float gain = static_cast<float>(i) / fadeFrames;
        for (UINT32 ch = 0; ch < channels; ch++) {
            audioData[i * channels + ch] *= gain;
        }
    }

    // Fade out
    UINT32 fadeStart = totalFrames - fadeFrames;
    for (UINT32 i = fadeStart; i < totalFrames; i++) {
        float gain = static_cast<float>(totalFrames - i) / fadeFrames;
        for (UINT32 ch = 0; ch < channels; ch++) {
            audioData[i * channels + ch] *= gain;
        }
    }
}

// Play audio using WASAPI
bool PlayAudio(const std::vector<float>& audioData, const WAVEFORMATEX* mixFormat,
               const std::vector<float>& leadIn = {},
               const std::vector<float>& leadOut = {}) {
    HRESULT hr;
    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    HANDLE eventHandle = nullptr;
    bool success = false;

    UINT32 channels = mixFormat->nChannels;

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&deviceEnumerator);
        if (FAILED(hr)) {
            PrintError("Failed to create device enumerator");
            break;
        }

        hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            PrintError("Failed to get default audio device");
            break;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
        if (FAILED(hr)) {
            PrintError("Failed to activate audio client");
            break;
        }

        eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            PrintError("Failed to create event");
            break;
        }

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     0, 0, mixFormat, nullptr);
        if (FAILED(hr)) {
            PrintError("Failed to initialize audio client");
            break;
        }

        hr = audioClient->SetEventHandle(eventHandle);
        if (FAILED(hr)) {
            PrintError("Failed to set event handle");
            break;
        }

        UINT32 bufferFrameCount;
        hr = audioClient->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr)) {
            PrintError("Failed to get buffer size");
            break;
        }

        hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
        if (FAILED(hr)) {
            PrintError("Failed to get render client");
            break;
        }

        hr = audioClient->Start();
        if (FAILED(hr)) {
            PrintError("Failed to start audio client");
            break;
        }

        // Play lead-in (BLE guard), main audio, then lead-out (BLE guard)
        const std::vector<float>* sources[] = { &leadIn, &audioData, &leadOut };
        bool playbackAborted = false;
        for (const auto* source : sources) {
            if (source->empty()) continue;

            size_t totalFrames = source->size() / channels;
            size_t frameIndex = 0;

            // Stall detection based on consecutive WAIT_TIMEOUT wakeups (event auto-reset guarantees ~BUFFER_WAIT_MS per timeout)
            int stallCount = 0;
            while (frameIndex < totalFrames) {
                DWORD waitResult = WaitForSingleObject(eventHandle, BUFFER_WAIT_MS);
                if (waitResult == WAIT_TIMEOUT) {
                    if (++stallCount >= RENDER_MAX_STALL_ITERATIONS) {
                        PrintError("Audio device stopped responding");
                        playbackAborted = true;
                        break;
                    }
                    continue;
                }
                // Any non-signaled return (WAIT_FAILED / WAIT_ABANDONED) means the audio thread is no longer
                // pumping; treat this as a hard failure rather than letting the outer loop report success.
                if (waitResult != WAIT_OBJECT_0) {
                    playbackAborted = true;
                    break;
                }

                UINT32 numFramesPadding;
                hr = audioClient->GetCurrentPadding(&numFramesPadding);
                if (FAILED(hr)) {
                    playbackAborted = true;
                    break;
                }

                UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
                if (numFramesAvailable == 0) continue;

                UINT32 framesToWrite = static_cast<UINT32>(
                    (std::min)(static_cast<size_t>(numFramesAvailable), totalFrames - frameIndex)
                );

                BYTE* buffer;
                hr = renderClient->GetBuffer(framesToWrite, &buffer);
                if (FAILED(hr)) {
                    playbackAborted = true;
                    break;
                }

                size_t byteCount = framesToWrite * mixFormat->nBlockAlign;
                memcpy(buffer, &(*source)[frameIndex * channels], byteCount);

                hr = renderClient->ReleaseBuffer(framesToWrite, 0);
                if (FAILED(hr)) {
                    playbackAborted = true;
                    break;
                }

                frameIndex += framesToWrite;
                stallCount = 0;
            }
            if (playbackAborted) break;
        }
        if (playbackAborted) {
            audioClient->Stop();
            break;
        }

        // Wait for buffered samples to actually play out, with a hard cap so a stuck device cannot hang the process
        UINT32 numFramesPadding = 0;
        DWORD drainElapsed = 0;
        bool drainedCleanly = false;
        while (drainElapsed < DRAIN_TIMEOUT_MS) {
            Sleep(DRAIN_POLL_MS);
            drainElapsed += DRAIN_POLL_MS;
            hr = audioClient->GetCurrentPadding(&numFramesPadding);
            if (FAILED(hr)) break;
            if (numFramesPadding == 0) {
                drainedCleanly = true;
                break;
            }
        }

        // Only honor the trailing pad when drain completed normally; skip it on timeout to avoid compounding the delay
        if (drainedCleanly) Sleep(DRAIN_WAIT_MS);

        audioClient->Stop();
        success = true;

    } while (false);

    // Release resources
    if (renderClient) renderClient->Release();
    if (audioClient) audioClient->Release();
    if (device) device->Release();
    if (deviceEnumerator) deviceEnumerator->Release();
    if (eventHandle) CloseHandle(eventHandle);

    return success;
}

// Parse a subset of TOML (sections + bool/float key-value) into config.
//
// Unknown sections and keys are silently ignored.
// Returns false only if the file cannot be opened; parse warnings go to stderr.
static bool ParseTomlFile(const wchar_t* path, AppConfig& config) {
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        return false;
    }
    // Hard cap to keep a hostile or accidental huge TOML from exhausting memory
    constexpr DWORD MAX_TOML_SIZE = 1024 * 1024;
    if (fileSize > MAX_TOML_SIZE) {
        CloseHandle(hFile);
        char nameBuf[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, PathFindFileNameW(path), -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
        std::cerr << "Warning: " << nameBuf << ": config file too large; ignored" << std::endl;
        return false;
    }
    std::string content(fileSize, '\0');
    DWORD bytesRead = 0;
    bool ok = (fileSize == 0) || ReadFile(hFile, &content[0], fileSize, &bytesRead, nullptr);
    CloseHandle(hFile);
    if (!ok) return false;
    content.resize(bytesRead);

    std::string section;
    size_t lineStart = 0;
    int lineNum = 0;

    auto trim = [](const std::string& s) -> std::string {
        const char* ws = " \t\r";
        size_t a = s.find_first_not_of(ws);
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(ws);
        return s.substr(a, b - a + 1);
    };

    while (lineStart < content.size()) {
        size_t lineEnd = content.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = content.size();
        std::string line = trim(content.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;
        lineNum++;

        if (line.empty() || line[0] == '#') continue;

        if (line[0] == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        size_t hash = val.find('#');
        if (hash != std::string::npos) val = trim(val.substr(0, hash));

        auto parseBool = [&](bool& dest) {
            if (val == "true") dest = true;
            else if (val == "false") dest = false;
            else std::cerr << "Warning: config line " << lineNum << ": invalid bool '" << val << "'" << std::endl;
        };
        // Parse and range-check a float value.
        // Rejects non-finite values and values outside [minVal, maxVal]; keeps the existing default on failure.
        auto parseFloat = [&](float& dest, float minVal, float maxVal) {
            char* end;
            float v = strtof(val.c_str(), &end);
            if (end == val.c_str() || !std::isfinite(v) || v < minVal || v > maxVal) {
                std::cerr << "Warning: config line " << lineNum << ": invalid value '" << val << "'" << std::endl;
                return;
            }
            dest = v;
        };

        if (section == "guard") {
            if      (key == "enabled")          parseBool(config.guardEnabled);
            else if (key == "frequency")        parseFloat(config.guardFrequency, 20.0f, 20000.0f);
            else if (key == "amplitude")        parseFloat(config.guardAmplitude, 0.0f, 1.0f);
            else if (key == "lead_in_duration") parseFloat(config.leadInDuration, 0.0f, 10.0f);
            else if (key == "lead_out_duration") parseFloat(config.leadOutDuration, 0.0f, 10.0f);
        }
        else if (section == "loudness") {
            if      (key == "enabled")      parseBool(config.loudnessEnabled);
            else if (key == "target")       parseFloat(config.loudnessTarget, -70.0f, 0.0f);
            else if (key == "peak_ceiling") parseFloat(config.loudnessPeakCeiling, 0.001f, 1.0f);
        }
    }
    return true;
}

// Load configuration from minply.toml and minply.local.toml in the executable directory.
//
// Missing files are silently skipped. Returns default AppConfig if both are absent.
static AppConfig LoadConfig() {
    AppConfig config;

    wchar_t exePath[MAX_PATH] = {};
    DWORD pathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH) return config;

    if (!PathRemoveFileSpecW(exePath)) return config;

    std::wstring dir(exePath);
    dir += L'\\';
    ParseTomlFile((dir + L"minply.toml").c_str(), config);
    ParseTomlFile((dir + L"minply.local.toml").c_str(), config);

    return config;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 2) {
        PrintError("Invalid arguments");
        std::cerr << "Usage: minply.exe [audio file path | -]" << std::endl;
        return ERR_INVALID_ARGS;
    }

    AppConfig config = LoadConfig();

    // Load audio data into buffer
    //
    // Argument resolution:
    //   - argc == 1            : read from stdin if piped, else exit silently
    //   - argv[1] == "-"       : read from stdin (error if empty)
    //   - argv[1] == file path : read from file
    std::vector<BYTE> inputData;
    if (argc == 1) {
        if (!ReadAllStdin(inputData)) {
            return EXIT_SUCCESS;
        }
    }
    else if (wcscmp(argv[1], L"-") == 0) {
        if (!ReadAllStdin(inputData)) {
            PrintError("No input data on stdin");
            return ERR_FILE_NOT_FOUND;
        }
    }
    else {
        const wchar_t* filePath = argv[1];
        if (GetFileAttributesW(filePath) == INVALID_FILE_ATTRIBUTES) {
            PrintError("File not found");
            return ERR_FILE_NOT_FOUND;
        }
        HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            PrintError("File not found");
            return ERR_FILE_NOT_FOUND;
        }
        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile, &fileSize)) {
            CloseHandle(hFile);
            PrintError("Failed to read file");
            return ERR_FILE_NOT_FOUND;
        }
        if (fileSize.QuadPart == 0) {
            CloseHandle(hFile);
            PrintError("File is empty");
            return ERR_FILE_NOT_FOUND;
        }
        // Guard against 4GB+ files: ReadFile takes DWORD, and notification sounds
        // never approach this size in practice
        if (fileSize.QuadPart > MAXDWORD) {
            CloseHandle(hFile);
            PrintError("File too large");
            return ERR_DECODE_FAILED;
        }
        inputData.resize(static_cast<size_t>(fileSize.QuadPart));
        DWORD toRead = static_cast<DWORD>(fileSize.QuadPart);
        DWORD bytesRead = 0;
        bool readOk = ReadFile(hFile, inputData.data(), toRead, &bytesRead, nullptr);
        CloseHandle(hFile);
        if (!readOk || bytesRead != toRead) {
            PrintError("Failed to read file");
            return ERR_FILE_NOT_FOUND;
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        PrintError("Failed to initialize COM");
        return ERR_WASAPI_INIT;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        PrintError("Failed to initialize Media Foundation");
        CoUninitialize();
        return ERR_DECODE_FAILED;
    }

    int exitCode = EXIT_SUCCESS;

    WAVEFORMATEX* mixFormat = nullptr;
    if (!GetDeviceMixFormat(&mixFormat)) {
        PrintError("Failed to get device format");
        exitCode = ERR_WASAPI_INIT;
        MFShutdown();
    }
    else {
        const BYTE* inputBytes = inputData.data();
        size_t inputSize = inputData.size();

        std::vector<float> decodedData;
        bool decoded = false;

        // Dispatch by magic bytes to avoid unnecessary decoder attempts
        if (inputSize >= 12 && memcmp(inputBytes, "RIFF", 4) == 0 && memcmp(inputBytes + 8, "WAVE", 4) == 0) {
            decoded = TryReadWavBuffer(inputBytes, inputSize, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
        }
        if (!decoded && inputSize >= 4 && memcmp(inputBytes, "OggS", 4) == 0) {
            decoded = TryDecodeOpusBuffer(inputBytes, inputSize, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
        }
        if (!decoded) {
            decoded = DecodeAudioBuffer(inputBytes, inputSize, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
        }

        MFShutdown();

        if (!decoded) {
            PrintError("Failed to decode audio");
            exitCode = ERR_DECODE_FAILED;
        }
        else {
            if (config.loudnessEnabled) {
                NormalizeLoudness(decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels,
                                  config.loudnessTarget, config.loudnessPeakCeiling);
            }

            ApplyFade(decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);

            std::vector<float> leadIn, leadOut;
            if (config.guardEnabled) {
                leadIn = GenerateBleGuard(mixFormat->nSamplesPerSec, mixFormat->nChannels,
                                          config.leadInDuration, config.guardFrequency, config.guardAmplitude);
                leadOut = GenerateBleGuard(mixFormat->nSamplesPerSec, mixFormat->nChannels,
                                           config.leadOutDuration, config.guardFrequency, config.guardAmplitude);
            }

            if (!PlayAudio(decodedData, mixFormat, leadIn, leadOut)) {
                PrintError("Failed to play audio");
                exitCode = ERR_PLAYBACK_FAILED;
            }
        }

        CoTaskMemFree(mixFormat);
    }

    CoUninitialize();

    return exitCode;
}
