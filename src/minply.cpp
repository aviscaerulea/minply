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
        if (pos + n > size) return false;
        memcpy(dst, data + pos, n);
        pos += n;
        return true;
    };
    // バッファ上で n バイト前進する（読み捨て）
    auto skipBytes = [&](size_t n) -> bool {
        if (pos + n > size) return false;
        pos += n;
        return true;
    };

    // Validate RIFF/WAVE header
    char header[12];
    if (!readBytes(header, 12)) return false;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) return false;

    // Find fmt chunk
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

    // Normalize format tag
    WORD actualFormatTag = fmt.Format.wFormatTag;
    if (actualFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt.Format.cbSize >= 22) {
        actualFormatTag = *reinterpret_cast<const WORD*>(&fmt.SubFormat);
    }

    if (actualFormatTag != WAVE_FORMAT_PCM && actualFormatTag != WAVE_FORMAT_IEEE_FLOAT) return false;
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
    if (pos + dataSize > size) return false;

    // Convert PCM samples to float32
    UINT32 bytesPerSample = fmt.Format.wBitsPerSample / 8;
    UINT32 totalSamples = dataSize / bytesPerSample;
    audioData.resize(totalSamples);

    const BYTE* rawData = data + pos;

    if (actualFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt.Format.wBitsPerSample == 32) {
        // float32 WAV - direct copy
        memcpy(audioData.data(), rawData, dataSize);
    }
    else if (actualFormatTag == WAVE_FORMAT_PCM) {
        // int PCM - convert to float32
        if (fmt.Format.wBitsPerSample == 16) {
            const int16_t* samples = reinterpret_cast<const int16_t*>(rawData);
            for (UINT32 i = 0; i < totalSamples; i++) {
                audioData[i] = static_cast<float>(samples[i]) / 32768.0f;
            }
        }
        else if (fmt.Format.wBitsPerSample == 24) {
            for (UINT32 i = 0; i < totalSamples; i++) {
                int32_t sample = (rawData[i * 3] << 8) | (rawData[i * 3 + 1] << 16) | (rawData[i * 3 + 2] << 24);
                sample >>= 8; // sign extend
                audioData[i] = static_cast<float>(sample) / 8388608.0f;
            }
        }
        else if (fmt.Format.wBitsPerSample == 32) {
            const int32_t* samples = reinterpret_cast<const int32_t*>(rawData);
            for (UINT32 i = 0; i < totalSamples; i++) {
                audioData[i] = static_cast<float>(samples[i]) / 2147483648.0f;
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
    const int opusSampleRate = 48000;
    std::vector<float> decodedFloat;
    int packetCount = 0;

    // Feed entire buffer in 64KB chunks (ogg_sync_buffer takes long)
    const size_t CHUNK_SIZE = 65536;
    size_t offset = 0;
    while (offset < size) {
        size_t toWrite = (std::min)(CHUNK_SIZE, size - offset);
        char* buf = ogg_sync_buffer(&oy, static_cast<long>(toWrite));
        if (!buf) break;
        memcpy(buf, data + offset, toWrite);
        ogg_sync_wrote(&oy, static_cast<long>(toWrite));
        offset += toWrite;
    }

    // Opus stream structure: packet 1 = OpusHead, packet 2 = OpusTags, packet 3+ = audio data
    while (ogg_sync_pageout(&oy, &og) == 1) {
        if (!streamInitialized) {
            ogg_stream_init(&os, ogg_page_serialno(&og));
            streamInitialized = true;
        }
        ogg_stream_pagein(&os, &og);

        while (ogg_stream_packetout(&os, &op) == 1) {
            packetCount++;

            if (packetCount == 1) {
                // OpusHead packet (format and channel information)
                if (op.bytes >= 19 && memcmp(op.packet, "OpusHead", 8) == 0) {
                    opusChannels = op.packet[9];
                    int error;
                    decoder = opus_decoder_create(opusSampleRate, opusChannels, &error);
                    if (error != OPUS_OK || !decoder) {
                        if (streamInitialized) ogg_stream_clear(&os);
                        ogg_sync_clear(&oy);
                        return false;
                    }
                }
                else {
                    // Not an Opus stream
                    if (decoder) opus_decoder_destroy(decoder);
                    if (streamInitialized) ogg_stream_clear(&os);
                    ogg_sync_clear(&oy);
                    return false;
                }
            }
            else if (packetCount == 2) {
                // OpusTags packet (metadata, skip)
                continue;
            }
            else {
                // Audio packet
                if (decoder) {
                    const int MAX_FRAME_SIZE = 5760;  // 120ms at 48kHz
                    float pcmBuffer[MAX_FRAME_SIZE * 8];  // Support up to 8 channels
                    int frameSize = opus_decode_float(decoder, op.packet, op.bytes,
                                                      pcmBuffer, MAX_FRAME_SIZE, 0);
                    if (frameSize > 0) {
                        size_t sampleCount = static_cast<size_t>(frameSize) * opusChannels;
                        decodedFloat.insert(decodedFloat.end(), pcmBuffer, pcmBuffer + sampleCount);
                    }
                }
            }
        }
    }

    if (decoder) opus_decoder_destroy(decoder);
    if (streamInitialized) ogg_stream_clear(&os);
    ogg_sync_clear(&oy);

    if (decodedFloat.empty()) return false;

    // Format conversion
    audioData = ConvertFormat(decodedFloat, opusSampleRate, opusChannels,
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
        // Wrap memory buffer as a seekable COM IStream
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

        // Configure output media type (PCM float)
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

        // Read all samples
        while (true) {
            DWORD flags = 0;
            IMFSample* sample = nullptr;

            hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                   nullptr, &flags, nullptr, &sample);
            if (FAILED(hr)) break;

            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
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

    // Pre-reserve 1MB to reduce reallocation overhead for typical notification sounds
    buffer.reserve(1024 * 1024);

    BYTE chunk[65536];
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
    ebur128_add_frames_float(state, audioData.data(), frames);

    double loudness = 0.0;
    int result = ebur128_loudness_global(state, &loudness);
    ebur128_destroy(&state);

    if (result != EBUR128_SUCCESS || std::isinf(loudness)) return;

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
        // Create device enumerator
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&deviceEnumerator);
        if (FAILED(hr)) {
            PrintError("Failed to create device enumerator");
            break;
        }

        // Get default audio device
        hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            PrintError("Failed to get default audio device");
            break;
        }

        // Activate audio client
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
        if (FAILED(hr)) {
            PrintError("Failed to activate audio client");
            break;
        }

        // Create event
        eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            PrintError("Failed to create event");
            break;
        }

        // Initialize audio client with device's mix format
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     0, 0, mixFormat, nullptr);
        if (FAILED(hr)) {
            PrintError("Failed to initialize audio client");
            break;
        }

        // Set event handle
        hr = audioClient->SetEventHandle(eventHandle);
        if (FAILED(hr)) {
            PrintError("Failed to set event handle");
            break;
        }

        // Get buffer size
        UINT32 bufferFrameCount;
        hr = audioClient->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr)) {
            PrintError("Failed to get buffer size");
            break;
        }

        // Get render client
        hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
        if (FAILED(hr)) {
            PrintError("Failed to get render client");
            break;
        }

        // Start playback
        hr = audioClient->Start();
        if (FAILED(hr)) {
            PrintError("Failed to start audio client");
            break;
        }

        // Play lead-in (BLE guard), main audio, then lead-out (BLE guard)
        const std::vector<float>* sources[] = { &leadIn, &audioData, &leadOut };
        for (const auto* source : sources) {
            if (source->empty()) continue;

            size_t totalFrames = source->size() / channels;
            size_t frameIndex = 0;

            while (frameIndex < totalFrames) {
                DWORD waitResult = WaitForSingleObject(eventHandle, BUFFER_WAIT_MS);
                if (waitResult == WAIT_TIMEOUT) continue;
                if (waitResult != WAIT_OBJECT_0) break;

                UINT32 numFramesPadding;
                hr = audioClient->GetCurrentPadding(&numFramesPadding);
                if (FAILED(hr)) break;

                UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
                if (numFramesAvailable == 0) continue;

                UINT32 framesToWrite = static_cast<UINT32>(
                    (std::min)(static_cast<size_t>(numFramesAvailable), totalFrames - frameIndex)
                );

                BYTE* buffer;
                hr = renderClient->GetBuffer(framesToWrite, &buffer);
                if (FAILED(hr)) break;

                size_t byteCount = framesToWrite * mixFormat->nBlockAlign;
                memcpy(buffer, &(*source)[frameIndex * channels], byteCount);

                hr = renderClient->ReleaseBuffer(framesToWrite, 0);
                if (FAILED(hr)) break;

                frameIndex += framesToWrite;
            }
            if (FAILED(hr)) break;
        }

        // Wait for all data to finish playing
        UINT32 numFramesPadding;
        do {
            Sleep(10);
            hr = audioClient->GetCurrentPadding(&numFramesPadding);
        } while (SUCCEEDED(hr) && numFramesPadding > 0);

        // Wait for device buffer to drain
        Sleep(DRAIN_WAIT_MS);

        // Stop playback
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
        auto parseFloat = [&](float& dest) {
            char* end;
            float v = strtof(val.c_str(), &end);
            if (end != val.c_str()) dest = v;
            else std::cerr << "Warning: config line " << lineNum << ": invalid float '" << val << "'" << std::endl;
        };

        if (section == "guard") {
            if      (key == "enabled")          parseBool(config.guardEnabled);
            else if (key == "frequency")        parseFloat(config.guardFrequency);
            else if (key == "amplitude")        parseFloat(config.guardAmplitude);
            else if (key == "lead_in_duration") parseFloat(config.leadInDuration);
            else if (key == "lead_out_duration") parseFloat(config.leadOutDuration);
        } else if (section == "loudness") {
            if      (key == "enabled")      parseBool(config.loudnessEnabled);
            else if (key == "target")       parseFloat(config.loudnessTarget);
            else if (key == "peak_ceiling") parseFloat(config.loudnessPeakCeiling);
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
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) return config;
    *(lastSlash + 1) = L'\0';

    std::wstring dir(exePath);
    ParseTomlFile((dir + L"minply.toml").c_str(), config);
    ParseTomlFile((dir + L"minply.local.toml").c_str(), config);

    return config;
}

int wmain(int argc, wchar_t* argv[]) {
    // Validate argument count
    if (argc > 2) {
        PrintError("Invalid arguments");
        std::cerr << "Usage: minply.exe [audio file path | -]" << std::endl;
        return ERR_INVALID_ARGS;
    }

    // Load configuration
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
        if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
            CloseHandle(hFile);
            PrintError("File not found");
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

    // Initialize COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        PrintError("Failed to initialize COM");
        return ERR_WASAPI_INIT;
    }

    // Initialize Media Foundation
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        PrintError("Failed to initialize Media Foundation");
        CoUninitialize();
        return ERR_DECODE_FAILED;
    }

    int exitCode = EXIT_SUCCESS;

    // Get device mix format
    WAVEFORMATEX* mixFormat = nullptr;
    if (!GetDeviceMixFormat(&mixFormat)) {
        PrintError("Failed to get device format");
        exitCode = ERR_WASAPI_INIT;
        MFShutdown();
    }
    else {
        const BYTE* buf = inputData.data();
        size_t sz = inputData.size();

        // Decode audio buffer
        std::vector<float> decodedData;
        bool decoded = false;

        // Dispatch by magic bytes to avoid unnecessary decoder attempts
        if (sz >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0) {
            decoded = TryReadWavBuffer(buf, sz, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
        }
        if (!decoded && sz >= 4 && memcmp(buf, "OggS", 4) == 0) {
            decoded = TryDecodeOpusBuffer(buf, sz, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
        }
        if (!decoded) {
            decoded = DecodeAudioBuffer(buf, sz, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
        }

        // MF is no longer needed after decode
        MFShutdown();

        if (!decoded) {
            PrintError("Failed to decode audio");
            exitCode = ERR_DECODE_FAILED;
        }
        else {
            // Normalize perceived loudness (EBU R128) to unify volume across sources
            if (config.loudnessEnabled) {
                NormalizeLoudness(decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels,
                                  config.loudnessTarget, config.loudnessPeakCeiling);
            }

            // Apply fade to prevent click noise
            ApplyFade(decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);

            // Generate BLE guard tone buffers
            std::vector<float> leadIn, leadOut;
            if (config.guardEnabled) {
                leadIn = GenerateBleGuard(mixFormat->nSamplesPerSec, mixFormat->nChannels,
                                          config.leadInDuration, config.guardFrequency, config.guardAmplitude);
                leadOut = GenerateBleGuard(mixFormat->nSamplesPerSec, mixFormat->nChannels,
                                           config.leadOutDuration, config.guardFrequency, config.guardAmplitude);
            }

            // Play with BLE guard tones to prevent BLE device from entering power-saving mode
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
