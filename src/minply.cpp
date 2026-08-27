/*
 * minply - Minimal Audio Player
 *
 * Lightweight and fast audio player with BLE receiver lag compensation
 *
 * Usage:
 *   minply.exe [--session <id>] [audio file path | -]
 *
 * Features:
 *   - Instantly plays MP3, WAV, AAC, FLAC, Opus and other audio files
 *   - Accepts audio data from stdin (no argument or - as argument)
 *   - Plays inaudible 19kHz guard tone before/after audio (BLE anti-clipping)
 *   - --session <id> keeps a linger server alive so consecutive playbacks of the
 *     same session skip the lead-in entirely (uninterrupted guard tone)
 *   - Exits immediately after playback completes
 *
 * Dependencies:
 *   - Windows Media Foundation: Audio decoder
 *   - libopus: Opus audio codec decoder
 *   - libogg: Ogg container format parser
 *   - libebur128: EBU R128 loudness measurement
 *   - WASAPI: Windows audio output
 *
 * Build:
 *   task build (vcpkg installs the x64-windows-static dependencies, then
 *   build.ps1 invokes cl.exe directly; C++17 / MSVC / x64)
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
#include <cstdint>
#include <atomic>
#include <utility>
#include <new>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
// Note: opus.lib, ogg.lib and ebur128.lib are linked via build.ps1

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
constexpr float LOUDNESS_PEAK_CEILING = 0.891f;  // Sample peak ceiling (-1dBFS); prevents clipping after loudness gain
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

// Linger server (--session) parameters
constexpr float  LINGER_DURATION          = 30.0f;   // Keep-alive seconds after the last playback
constexpr DWORD  PIPE_CONNECT_RETRY_MS    = 50;      // Client retry interval while the server pipe is absent
constexpr DWORD  CLIENT_SPAWN_DEADLINE_MS = 5000;    // Give up waiting for a newly spawned server after this
constexpr DWORD  CLIENT_QUEUE_DEADLINE_MS = 120000;  // Give up waiting behind other queued playbacks after this
constexpr DWORD  PIPE_BUFFER_HINT         = 65536;   // Named pipe in/out buffer size hint
constexpr DWORD  PIPE_IO_TIMEOUT_MS       = 30000;   // Per-operation cap so a dead client cannot block the server forever
constexpr size_t PIPE_CHUNK               = 65536;   // Transfer chunk size over the session pipe
constexpr size_t SESSION_ID_MAX           = 64;      // Session id length cap (kernel object name safety)
constexpr uint64_t PIPE_MAX_DATA          = 256ULL * 1024 * 1024;  // Request payload cap; far above any notification sound, keeps a hostile client from forcing huge allocations
constexpr DWORD  CLIENT_RESPONSE_TIMEOUT_MS = 600000;  // Client-side cap on waiting for its own playback result (covers long audio)
constexpr DWORD  SERVER_PLAY_MARGIN_MS    = 30000;   // Extra wait margin for the render thread to finish one audio
constexpr DWORD  CONNECT_POLL_MS          = 100;     // Slice length while waiting for a client, so device loss is noticed promptly
constexpr DWORD  RENDER_JOIN_TIMEOUT_MS   = 10000;   // Hard cap on waiting for the render thread during shutdown
constexpr DWORD  SERVER_INIT_TIMEOUT_MS   = 15000;   // Cap on render-thread startup; a WASAPI call that hangs must not strand connected clients

// Session pipe protocol
constexpr char     PIPE_MAGIC[4]      = { 'M', 'P', 'R', 'Q' };  // "minply request"; guards against a foreign writer on the pipe
constexpr uint32_t PIPE_PROTO_VERSION = 1;       // Bump whenever the header layout or the request/ack/response exchange changes
constexpr BYTE     PIPE_ACK_BYTE      = 0x01;    // Sent once the request is fully read; tells the client the audio is now the server's responsibility

// Request header sent by the client, followed by dataSize bytes of encoded audio
#pragma pack(push, 1)
struct PipeRequestHeader {
    char     magic[4];
    uint32_t version;
    uint64_t dataSize;
};

// Playback result returned by the server after the audio has finished
struct PipeResponse {
    int32_t exitCode;
};
#pragma pack(pop)

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
    float lingerDuration      = LINGER_DURATION;
};

// Print error message to stderr
void PrintError(const char* message) {
    std::cerr << "Error: " << message << std::endl;
}

// Validate a session id
//
// The id becomes part of a kernel pipe name, so only unreserved characters are
// accepted. A Claude Code session_id (UUID) fits within these limits.
static bool IsValidSessionId(const wchar_t* id) {
    size_t len = wcslen(id);
    if (len == 0 || len > SESSION_ID_MAX) return false;

    for (size_t i = 0; i < len; i++) {
        wchar_t c = id[i];
        bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')
               || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-';
        if (!ok) return false;
    }
    return true;
}

// Build the named pipe path for a session id
static std::wstring PipeName(const wchar_t* id) {
    return std::wstring(L"\\\\.\\pipe\\minply-") + id;
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

    // Decide acceptance before allocating: a bit depth this decoder cannot
    // convert must leave audioData untouched, otherwise the zero-filled buffer
    // survives into the Media Foundation fallback as leading silence.
    bool bitDepthSupported =
        (actualFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt.Format.wBitsPerSample == 32)
        || (actualFormatTag == WAVE_FORMAT_PCM
            && (fmt.Format.wBitsPerSample == 16 || fmt.Format.wBitsPerSample == 24
                || fmt.Format.wBitsPerSample == 32));
    if (!bitDepthSupported) return false;

    UINT32 bytesPerSample = fmt.Format.wBitsPerSample / 8;
    UINT32 totalSamples = dataSize / bytesPerSample;
    // A data chunk length that is not a whole number of samples is malformed:
    // the float32 path memcpy's dataSize bytes into a buffer sized by sample
    // count, and a sub-sample length would return success with no audio at all.
    // Reject here and let the Media Foundation fallback handle the file.
    if ((dataSize % bytesPerSample) != 0 || totalSamples == 0) return false;
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
        // The float division rounds twice, so srcIndex can land one frame past
        // the true value on long inputs. Equal-rate paths have zero headroom
        // (dstFrames == srcFrames), so clamp like idx1 to stay in bounds.
        // frac may reach 1.0, but then idx1 == idx0 makes s1 == s0 and the
        // interpolated value stays the last sample.
        size_t idx0 = (std::min)(static_cast<size_t>(srcIndex), srcFrames - 1);
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

// Decode encoded audio bytes into float PCM at the device mix format
//
// Dispatches by magic bytes (WAV -> Opus/Ogg -> Media Foundation fallback) to
// avoid paying for decoder attempts that cannot succeed.
static bool DecodeInput(const BYTE* data, size_t size, const WAVEFORMATEX* mixFormat,
                        std::vector<float>& decodedData) {
    bool decoded = false;

    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0) {
        decoded = TryReadWavBuffer(data, size, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
    }
    if (!decoded && size >= 4 && memcmp(data, "OggS", 4) == 0) {
        decoded = TryDecodeOpusBuffer(data, size, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
    }
    if (!decoded) {
        // A failed decoder may have left partial output behind; start the
        // fallback from an empty buffer so no silence is prepended to the audio
        decodedData.clear();
        decoded = DecodeAudioBuffer(data, size, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
    }

    return decoded;
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
// then clamps gain if sample peak would exceed peakCeiling.
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

// Apply loudness normalization (if enabled) and fade in/out to decoded audio
static void PostProcess(std::vector<float>& audioData, const WAVEFORMATEX* mixFormat,
                        const AppConfig& config) {
    if (config.loudnessEnabled) {
        NormalizeLoudness(audioData, mixFormat->nSamplesPerSec, mixFormat->nChannels,
                          config.loudnessTarget, config.loudnessPeakCeiling);
    }
    ApplyFade(audioData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
}

// Bundled WASAPI render objects for a single shared-mode session
struct RenderSession {
    IMMDeviceEnumerator* enumerator   = nullptr;
    IMMDevice*           device       = nullptr;
    IAudioClient*        audioClient  = nullptr;
    IAudioRenderClient*  renderClient = nullptr;
    HANDLE               eventHandle  = nullptr;
    UINT32               bufferFrames = 0;
};

// Release all objects held by the session (idempotent)
static void CloseRenderSession(RenderSession& s) {
    if (s.renderClient) { s.renderClient->Release(); s.renderClient = nullptr; }
    if (s.audioClient)  { s.audioClient->Release();  s.audioClient = nullptr; }
    if (s.device)       { s.device->Release();       s.device = nullptr; }
    if (s.enumerator)   { s.enumerator->Release();   s.enumerator = nullptr; }
    if (s.eventHandle)  { CloseHandle(s.eventHandle); s.eventHandle = nullptr; }
    s.bufferFrames = 0;
}

// Open a shared-mode event-driven render session on the default device
//
// On failure prints an error and releases partially acquired objects.
static bool OpenRenderSession(RenderSession& s, const WAVEFORMATEX* mixFormat) {
    HRESULT hr;

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&s.enumerator);
        if (FAILED(hr)) {
            PrintError("Failed to create device enumerator");
            break;
        }

        hr = s.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &s.device);
        if (FAILED(hr)) {
            PrintError("Failed to get default audio device");
            break;
        }

        hr = s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.audioClient);
        if (FAILED(hr)) {
            PrintError("Failed to activate audio client");
            break;
        }

        s.eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!s.eventHandle) {
            PrintError("Failed to create event");
            break;
        }

        hr = s.audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                       0, 0, mixFormat, nullptr);
        if (FAILED(hr)) {
            PrintError("Failed to initialize audio client");
            break;
        }

        hr = s.audioClient->SetEventHandle(s.eventHandle);
        if (FAILED(hr)) {
            PrintError("Failed to set event handle");
            break;
        }

        hr = s.audioClient->GetBufferSize(&s.bufferFrames);
        if (FAILED(hr)) {
            PrintError("Failed to get buffer size");
            break;
        }

        hr = s.audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&s.renderClient);
        if (FAILED(hr)) {
            PrintError("Failed to get render client");
            break;
        }

        return true;

    } while (false);

    CloseRenderSession(s);
    return false;
}

// Play audio using WASAPI
bool PlayAudio(const std::vector<float>& audioData, const WAVEFORMATEX* mixFormat,
               const std::vector<float>& leadIn = {},
               const std::vector<float>& leadOut = {}) {
    HRESULT hr;
    RenderSession session;
    bool success = false;

    UINT32 channels = mixFormat->nChannels;

    do {
        if (!OpenRenderSession(session, mixFormat)) break;

        hr = session.audioClient->Start();
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
                DWORD waitResult = WaitForSingleObject(session.eventHandle, BUFFER_WAIT_MS);
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
                hr = session.audioClient->GetCurrentPadding(&numFramesPadding);
                if (FAILED(hr)) {
                    playbackAborted = true;
                    break;
                }

                UINT32 numFramesAvailable = session.bufferFrames - numFramesPadding;
                if (numFramesAvailable == 0) continue;

                UINT32 framesToWrite = static_cast<UINT32>(
                    (std::min)(static_cast<size_t>(numFramesAvailable), totalFrames - frameIndex)
                );

                BYTE* buffer;
                hr = session.renderClient->GetBuffer(framesToWrite, &buffer);
                if (FAILED(hr)) {
                    playbackAborted = true;
                    break;
                }

                size_t byteCount = framesToWrite * mixFormat->nBlockAlign;
                memcpy(buffer, &(*source)[frameIndex * channels], byteCount);

                hr = session.renderClient->ReleaseBuffer(framesToWrite, 0);
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
            session.audioClient->Stop();
            break;
        }

        // Wait for buffered samples to actually play out, with a hard cap so a stuck device cannot hang the process
        UINT32 numFramesPadding = 0;
        DWORD drainElapsed = 0;
        bool drainedCleanly = false;
        while (drainElapsed < DRAIN_TIMEOUT_MS) {
            Sleep(DRAIN_POLL_MS);
            drainElapsed += DRAIN_POLL_MS;
            hr = session.audioClient->GetCurrentPadding(&numFramesPadding);
            if (FAILED(hr)) break;
            if (numFramesPadding == 0) {
                drainedCleanly = true;
                break;
            }
        }

        // Only honor the trailing pad when drain completed normally; skip it on timeout to avoid compounding the delay
        if (drainedCleanly) Sleep(DRAIN_WAIT_MS);

        session.audioClient->Stop();
        success = true;

    } while (false);

    CloseRenderSession(session);

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

    // Windows editors add a UTF-8 BOM readily (PowerShell 5.1 Set-Content
    // -Encoding UTF8, VS Code utf8bom, old Notepad). Leaving it makes a
    // first-line section header unrecognized, and every key under that section
    // is then dropped without a warning.
    if (content.size() >= 3
        && static_cast<unsigned char>(content[0]) == 0xEF
        && static_cast<unsigned char>(content[1]) == 0xBB
        && static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

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
        else if (section == "linger") {
            if (key == "duration") parseFloat(config.lingerDuration, 1.0f, 600.0f);
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

// Read exactly size bytes from an overlapped pipe handle
//
// Both pipe ends are opened with FILE_FLAG_OVERLAPPED, so every transfer needs
// an OVERLAPPED structure. timeoutMs bounds the whole operation (not one chunk),
// so a peer trickling bytes cannot hold the connection indefinitely.
static bool PipeReadExact(HANDLE pipe, void* buf, size_t size, DWORD timeoutMs) {
    BYTE* dst = static_cast<BYTE*>(buf);
    size_t done = 0;
    ULONGLONG deadline = GetTickCount64() + timeoutMs;

    HANDLE ev = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;

    bool result = true;
    while (done < size) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) { result = false; break; }

        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);

        DWORD toRead = static_cast<DWORD>((std::min)(PIPE_CHUNK, size - done));
        DWORD got = 0;
        bool ok = false;

        if (ReadFile(pipe, dst + done, toRead, &got, &ov)) {
            ok = true;
        }
        else if (GetLastError() == ERROR_IO_PENDING) {
            DWORD remaining = static_cast<DWORD>(deadline - now);
            if (WaitForSingleObject(ev, remaining) == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(pipe, &ov, &got, FALSE) != 0;
            }
            else {
                // Reclaim the buffer before it goes out of scope
                CancelIo(pipe);
                GetOverlappedResult(pipe, &ov, &got, TRUE);
            }
        }

        if (!ok || got == 0) { result = false; break; }
        done += got;
    }

    CloseHandle(ev);
    return result;
}

// Write all bytes to an overlapped pipe handle
//
// Mirrors PipeReadExact: overlapped transfer with timeoutMs bounding the whole
// operation, keeping a dead or stalled peer from blocking this process.
static bool PipeWriteAll(HANDLE pipe, const void* buf, size_t size, DWORD timeoutMs) {
    const BYTE* src = static_cast<const BYTE*>(buf);
    size_t done = 0;
    ULONGLONG deadline = GetTickCount64() + timeoutMs;

    HANDLE ev = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;

    bool result = true;
    while (done < size) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) { result = false; break; }

        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);

        DWORD toWrite = static_cast<DWORD>((std::min)(PIPE_CHUNK, size - done));
        DWORD put = 0;
        bool ok = false;

        if (WriteFile(pipe, src + done, toWrite, &put, &ov)) {
            ok = true;
        }
        else if (GetLastError() == ERROR_IO_PENDING) {
            DWORD remaining = static_cast<DWORD>(deadline - now);
            if (WaitForSingleObject(ev, remaining) == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(pipe, &ov, &put, FALSE) != 0;
            }
            else {
                CancelIo(pipe);
                GetOverlappedResult(pipe, &ov, &put, TRUE);
            }
        }

        if (!ok || put == 0) { result = false; break; }
        done += put;
    }

    CloseHandle(ev);
    return result;
}

// Get the id string of the current default render endpoint
//
// WASAPI does not invalidate an existing stream when the default device merely
// changes (only when the endpoint disappears), so the linger server compares
// this id per request to notice a switch. Returns an empty string on failure.
static std::wstring GetDefaultEndpointId() {
    std::wstring id;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) {
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            LPWSTR raw = nullptr;
            if (SUCCEEDED(device->GetId(&raw))) {
                id = raw;
                CoTaskMemFree(raw);
            }
        }
    }

    if (device) device->Release();
    if (enumerator) enumerator->Release();
    return id;
}

// Check that the mix format is 32-bit IEEE float PCM with packed frames
//
// The render thread writes float samples assuming nBlockAlign == channels * 4;
// any other layout would overrun the WASAPI buffer, so refuse it up front.
static bool IsFloat32MixFormat(const WAVEFORMATEX* f) {
    // Zero channels or rate would divide by zero downstream, and a zero
    // nBlockAlign would otherwise satisfy the layout check below
    if (f->nChannels == 0 || f->nSamplesPerSec == 0) return false;
    if (f->wBitsPerSample != 32) return false;
    if (f->nBlockAlign != f->nChannels * sizeof(float)) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid the ksmedia.h dependency
        static const GUID ieeeFloat =
            { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return memcmp(&ext->SubFormat, &ieeeFloat, sizeof(GUID)) == 0;
    }
    return false;
}

// Shared state between the pipe thread (server main) and the render thread
struct ServerState {
    CRITICAL_SECTION    cs;
    std::vector<float>  pending;               // Decoded audio waiting to be picked up (guarded by cs)
    bool                hasPending = false;    // (guarded by cs)
    HANDLE              doneEvent  = nullptr;  // Auto-reset; signaled once per handed-off audio
    HANDLE              readyEvent = nullptr;  // Manual-reset; signaled after render init succeeds or fails
    int                 playResult = EXIT_SUCCESS;  // Result of the last audio; written before doneEvent
    UINT32              bufferFrames = 0;      // WASAPI buffer size; valid after readyEvent
    std::atomic<bool>   renderOk{false};
    std::atomic<bool>   stopRequested{false};
    std::atomic<bool>   deviceLost{false};
    const WAVEFORMATEX* mixFormat = nullptr;
    AppConfig           config;
};

// Render thread: owns one WASAPI stream for the whole server lifetime
//
// Fills the buffer with the guard tone while idle (digital silence when the guard
// is disabled) and switches to queued audio at the next buffer refill, so a waiting
// playback starts within one buffer period instead of paying a lead-in again.
// The initial lead-in must elapse before the first audio may start, because the
// receiver is still asleep when the server has just been spawned.
static DWORD WINAPI RenderThreadProc(LPVOID param) {
    ServerState* state = static_cast<ServerState*>(param);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        state->renderOk = false;
        SetEvent(state->readyEvent);
        return 0;
    }

    RenderSession session;
    if (!OpenRenderSession(session, state->mixFormat) || FAILED(session.audioClient->Start())) {
        CloseRenderSession(session);
        state->renderOk = false;
        SetEvent(state->readyEvent);
        CoUninitialize();
        return 0;
    }

    state->bufferFrames = session.bufferFrames;
    state->renderOk = true;
    SetEvent(state->readyEvent);

    const UINT32 channels = state->mixFormat->nChannels;
    const UINT32 rate     = state->mixFormat->nSamplesPerSec;
    const bool   guardOn  = state->config.guardEnabled;
    const float  amp      = state->config.guardAmplitude;
    const float  phaseInc = TWO_PI * state->config.guardFrequency / rate;

    std::vector<float> active;
    size_t activeFrames = 0;
    size_t activeIndex  = 0;
    bool   activePlaying = false;
    size_t leadInRemain = guardOn ? static_cast<size_t>(rate * state->config.leadInDuration) : 0;
    float  phase = 0.0f;
    int    stallCount = 0;

    while (!state->stopRequested) {
        DWORD waitResult = WaitForSingleObject(session.eventHandle, BUFFER_WAIT_MS);
        if (waitResult == WAIT_TIMEOUT) {
            // Consecutive timeouts mean the audio engine stopped pumping this stream
            if (++stallCount >= RENDER_MAX_STALL_ITERATIONS) {
                state->deviceLost = true;
                break;
            }
            continue;
        }
        if (waitResult != WAIT_OBJECT_0) {
            state->deviceLost = true;
            break;
        }
        stallCount = 0;

        UINT32 padding = 0;
        if (FAILED(session.audioClient->GetCurrentPadding(&padding))) {
            state->deviceLost = true;
            break;
        }

        UINT32 framesAvail = session.bufferFrames - padding;
        if (framesAvail == 0) continue;

        // Pick up the next queued audio only once the lead-in has been emitted
        if (!activePlaying && leadInRemain == 0) {
            EnterCriticalSection(&state->cs);
            if (state->hasPending) {
                active = std::move(state->pending);
                state->pending.clear();
                state->hasPending = false;
                activeFrames  = active.size() / channels;
                activeIndex   = 0;
                activePlaying = activeFrames > 0;
                // An empty result has nothing to render; report success at once
                // so the pipe thread does not wait out its whole margin
                if (!activePlaying) {
                    state->playResult = EXIT_SUCCESS;
                    SetEvent(state->doneEvent);
                }
            }
            LeaveCriticalSection(&state->cs);
        }

        BYTE* raw = nullptr;
        if (FAILED(session.renderClient->GetBuffer(framesAvail, &raw))) {
            state->deviceLost = true;
            break;
        }

        float* out = reinterpret_cast<float*>(raw);
        for (UINT32 f = 0; f < framesAvail; f++) {
            if (activePlaying) {
                for (UINT32 ch = 0; ch < channels; ch++) {
                    out[f * channels + ch] = active[activeIndex * channels + ch];
                }
                activeIndex++;
                if (activeIndex >= activeFrames) {
                    // Remaining frames of this refill fall through to the idle tone.
                    // The amplitude step is inaudible at -60dB, so no phase matching is needed.
                    activePlaying = false;
                    active.clear();
                    state->playResult = EXIT_SUCCESS;
                    SetEvent(state->doneEvent);
                }
            }
            else {
                float s = guardOn ? amp * sinf(phase) : 0.0f;
                for (UINT32 ch = 0; ch < channels; ch++) {
                    out[f * channels + ch] = s;
                }
                phase += phaseInc;
                if (phase >= TWO_PI) phase -= TWO_PI;
                if (leadInRemain > 0) leadInRemain--;
            }
        }

        if (FAILED(session.renderClient->ReleaseBuffer(framesAvail, 0))) {
            state->deviceLost = true;
            break;
        }
    }

    // Unblock a caller still waiting on audio that will never finish. Audio
    // queued but not yet picked up counts too: the stall detector can break out
    // of the loop without ever reaching the pickup block.
    EnterCriticalSection(&state->cs);
    bool orphaned = activePlaying || state->hasPending;
    state->hasPending = false;
    state->pending.clear();
    LeaveCriticalSection(&state->cs);

    if (orphaned) {
        state->playResult = ERR_PLAYBACK_FAILED;
        SetEvent(state->doneEvent);
    }

    session.audioClient->Stop();
    CloseRenderSession(session);
    CoUninitialize();
    return 0;
}

// Serve playback requests for a session and keep the receiver awake between them
//
// Owns the session pipe and one render thread, then shuts down after
// config.lingerDuration seconds without a client. Creating the pipe with
// FILE_FLAG_FIRST_PIPE_INSTANCE doubles as the single-server election: a second
// server for the same id fails here and exits quietly.
static int RunServer(const wchar_t* sessionId) {
    AppConfig config = LoadConfig();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return ERR_WASAPI_INIT;

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        CoUninitialize();
        return ERR_DECODE_FAILED;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    if (!GetDeviceMixFormat(&mixFormat)) {
        MFShutdown();
        CoUninitialize();
        return ERR_WASAPI_INIT;
    }
    // The render thread writes raw float frames; refuse a device whose mix
    // format has any other layout instead of corrupting the WASAPI buffer
    if (!IsFloat32MixFormat(mixFormat)) {
        CoTaskMemFree(mixFormat);
        MFShutdown();
        CoUninitialize();
        return ERR_WASAPI_INIT;
    }

    std::wstring pipePath = PipeName(sessionId);
    HANDLE pipe = CreateNamedPipeW(pipePath.c_str(),
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, PIPE_BUFFER_HINT, PIPE_BUFFER_HINT, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        // Another server already owns this session id
        CoTaskMemFree(mixFormat);
        MFShutdown();
        CoUninitialize();
        return EXIT_SUCCESS;
    }

    ServerState state;
    InitializeCriticalSection(&state.cs);
    state.doneEvent  = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    state.readyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    state.mixFormat  = mixFormat;
    state.config     = config;

    int exitCode = EXIT_SUCCESS;
    HANDLE renderThread = nullptr;
    HANDLE connEvent = nullptr;

    do {
        if (!state.doneEvent || !state.readyEvent) {
            exitCode = ERR_WASAPI_INIT;
            break;
        }

        connEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!connEvent) {
            exitCode = ERR_WASAPI_INIT;
            break;
        }

        renderThread = CreateThread(nullptr, 0, RenderThreadProc, &state, 0, nullptr);
        if (!renderThread) {
            exitCode = ERR_WASAPI_INIT;
            break;
        }

        // Bound the wait: a WASAPI call that hangs during startup would otherwise
        // strand clients, which can already connect to the pipe created above
        if (WaitForSingleObject(state.readyEvent, SERVER_INIT_TIMEOUT_MS) != WAIT_OBJECT_0
            || !state.renderOk) {
            exitCode = ERR_WASAPI_INIT;
            break;
        }

        const DWORD lingerMs = static_cast<DWORD>(config.lingerDuration * 1000.0f);
        const std::wstring startupDeviceId = GetDefaultEndpointId();

        while (true) {
            // Accept the next client, giving up once the linger window expires
            OVERLAPPED ov = {};
            ov.hEvent = connEvent;
            ResetEvent(connEvent);

            bool connected = false;
            bool shutdown = false;

            if (ConnectNamedPipe(pipe, &ov)) {
                connected = true;
            }
            else {
                DWORD err = GetLastError();
                if (err == ERROR_PIPE_CONNECTED) {
                    connected = true;
                }
                else if (err == ERROR_IO_PENDING) {
                    ULONGLONG deadline = GetTickCount64() + lingerMs;
                    // Poll in slices so a lost device ends the linger without waiting it out
                    while (true) {
                        if (state.deviceLost) { shutdown = true; break; }
                        ULONGLONG now = GetTickCount64();
                        if (now >= deadline) { shutdown = true; break; }
                        ULONGLONG left = deadline - now;
                        DWORD slice = static_cast<DWORD>((std::min)(static_cast<ULONGLONG>(CONNECT_POLL_MS), left));
                        DWORD w = WaitForSingleObject(connEvent, slice);
                        if (w == WAIT_OBJECT_0) { connected = true; break; }
                        if (w != WAIT_TIMEOUT) { shutdown = true; break; }
                    }
                    if (!connected) {
                        DWORD ignored = 0;
                        CancelIo(pipe);
                        GetOverlappedResult(pipe, &ov, &ignored, TRUE);
                    }
                }
                else {
                    shutdown = true;
                }
            }

            if (!connected || shutdown) break;

            // Bail out before taking on work the render thread can no longer do.
            // No ack has been sent, so the client is free to retry elsewhere.
            if (state.deviceLost) {
                DisconnectNamedPipe(pipe);
                break;
            }

            // A default-device switch does not invalidate the stream, so compare
            // endpoint ids per request and shut down on a change. The client's
            // retry then spawns a fresh server bound to the new device. An empty
            // id means the lookup failed; treat that as unknown rather than as a
            // change, which would otherwise recycle the server on every request.
            std::wstring currentDeviceId = GetDefaultEndpointId();
            if (!currentDeviceId.empty() && !startupDeviceId.empty()
                && currentDeviceId != startupDeviceId) {
                DisconnectNamedPipe(pipe);
                break;
            }

            // One request per connection
            PipeRequestHeader header = {};
            std::vector<BYTE> payload;
            bool requestOk = PipeReadExact(pipe, &header, sizeof(header), PIPE_IO_TIMEOUT_MS)
                          && memcmp(header.magic, PIPE_MAGIC, sizeof(PIPE_MAGIC)) == 0
                          && header.version == PIPE_PROTO_VERSION
                          && header.dataSize > 0
                          && header.dataSize <= PIPE_MAX_DATA;
            if (requestOk) {
                try {
                    payload.resize(static_cast<size_t>(header.dataSize));
                }
                catch (const std::bad_alloc&) {
                    requestOk = false;
                }
            }
            if (requestOk) {
                requestOk = PipeReadExact(pipe, payload.data(), payload.size(), PIPE_IO_TIMEOUT_MS);
            }
            if (!requestOk) {
                DisconnectNamedPipe(pipe);
                continue;
            }

            // Acknowledge before any long work. The client uses this to tell
            // "never received" (safe to retry) from "accepted and playing"
            // (retrying would play the same audio twice).
            if (!PipeWriteAll(pipe, &PIPE_ACK_BYTE, sizeof(PIPE_ACK_BYTE), PIPE_IO_TIMEOUT_MS)) {
                DisconnectNamedPipe(pipe);
                continue;
            }

            PipeResponse response = { EXIT_SUCCESS };
            bool renderWedged = false;
            std::vector<float> decoded;
            bool decoded_ok = false;
            // Decoding expands compressed audio by a large factor, so a payload
            // within the request cap can still exhaust memory; fail just this
            // request instead of letting the server process die
            try {
                decoded_ok = DecodeInput(payload.data(), payload.size(), mixFormat, decoded);
                if (decoded_ok) PostProcess(decoded, mixFormat, config);
            }
            catch (const std::bad_alloc&) {
                decoded_ok = false;
                decoded.clear();
            }

            if (!decoded_ok) {
                response.exitCode = ERR_DECODE_FAILED;
            }
            else {
                size_t frames = decoded.size() / mixFormat->nChannels;
                DWORD audioMs = static_cast<DWORD>(frames * 1000ULL / mixFormat->nSamplesPerSec);

                // The render thread's exit cleanup signals doneEvent only once,
                // so a request queued after it would never be signalled and
                // would burn the whole play margin before failing. Test
                // deviceLost inside the same critical section: in the reverse
                // order the cleanup still sees hasPending and signals.
                bool renderDead = false;
                EnterCriticalSection(&state.cs);
                renderDead = state.deviceLost;
                if (!renderDead) {
                    state.pending = std::move(decoded);
                    state.hasPending = true;
                }
                LeaveCriticalSection(&state.cs);

                if (renderDead) {
                    response.exitCode = ERR_PLAYBACK_FAILED;
                    renderWedged = true;
                }
                else {
                    DWORD w = WaitForSingleObject(state.doneEvent, audioMs + SERVER_PLAY_MARGIN_MS);
                    if (w != WAIT_OBJECT_0) {
                        // A timeout leaves the render thread in an unknown state (a late
                        // doneEvent would corrupt the next request), so stop serving
                        response.exitCode = ERR_PLAYBACK_FAILED;
                        renderWedged = true;
                    }
                    else {
                        // The render thread reports the outcome; losing the device
                        // afterwards ends the linger but does not undo a finished playback
                        response.exitCode = state.playResult;
                        if (state.deviceLost) renderWedged = true;

                        if (response.exitCode == EXIT_SUCCESS) {
                            // The render thread signals when the last frame is written, so the
                            // frames still sitting in the device buffer have yet to be heard.
                            // Hold the client until they drain, preserving the contract that
                            // minply exits only after its audio finished.
                            DWORD bufferMs = static_cast<DWORD>(
                                static_cast<ULONGLONG>(state.bufferFrames) * 1000ULL / mixFormat->nSamplesPerSec);
                            Sleep(bufferMs + DRAIN_WAIT_MS);
                        }
                    }
                }
            }

            if (PipeWriteAll(pipe, &response, sizeof(response), PIPE_IO_TIMEOUT_MS)) {
                // Wait for the client to read the response and close its end;
                // disconnecting earlier would discard the unread response, and
                // FlushFileBuffers has no timeout. The read fails with a broken
                // pipe once the client closes, bounded by the I/O timeout.
                BYTE closeProbe = 0;
                PipeReadExact(pipe, &closeProbe, 1, PIPE_IO_TIMEOUT_MS);
            }
            DisconnectNamedPipe(pipe);

            if (renderWedged) break;
        }

    } while (false);

    // Close the pipe before joining the render thread so a client arriving in
    // the shutdown window fails to connect immediately and retries against a
    // freshly spawned server instead of losing its audio
    CloseHandle(pipe);

    state.stopRequested = true;
    if (renderThread) {
        // A render thread stuck past the join timeout still uses this stack's
        // state; tearing it down would be a use-after-free, so exit hard instead
        if (WaitForSingleObject(renderThread, RENDER_JOIN_TIMEOUT_MS) != WAIT_OBJECT_0) {
            TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
        }
        CloseHandle(renderThread);
    }

    if (connEvent) CloseHandle(connEvent);
    if (state.doneEvent) CloseHandle(state.doneEvent);
    if (state.readyEvent) CloseHandle(state.readyEvent);
    DeleteCriticalSection(&state.cs);
    CoTaskMemFree(mixFormat);
    MFShutdown();
    CoUninitialize();

    return exitCode;
}

// Spawn a detached linger server process for the session
//
// Re-launches this executable with the internal --serve entry point. Losing a
// race to another client is harmless: the loser's server exits at once because
// the pipe it needs already exists.
static bool SpawnServer(const wchar_t* sessionId) {
    wchar_t exePath[MAX_PATH] = {};
    DWORD pathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH) return false;

    std::wstring cmdline = L"\"";
    cmdline += exePath;
    cmdline += L"\" --serve ";
    cmdline += sessionId;

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // DETACHED_PROCESS keeps the server off the caller's console so it outlives
    // the shell that started it. It must not be combined with CREATE_NO_WINDOW.
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, FALSE,
                        DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Connect to the session's server pipe, spawning the server when absent
//
// ERROR_PIPE_BUSY means another client is being served, which is how playbacks
// of one session end up serialized. queueDeadline caps the total queue wait
// across attempts. Returns INVALID_HANDLE_VALUE once a deadline passes, when
// the server cannot be spawned, or on any other connect error.
static HANDLE ConnectToServer(const wchar_t* sessionId, ULONGLONG queueDeadline) {
    std::wstring pipePath = PipeName(sessionId);
    ULONGLONG spawnDeadline = GetTickCount64() + CLIENT_SPAWN_DEADLINE_MS;
    bool spawned = false;

    while (true) {
        HANDLE pipe = CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            if (GetTickCount64() > queueDeadline) return INVALID_HANDLE_VALUE;
            WaitNamedPipeW(pipePath.c_str(), 1000);
            continue;
        }
        if (err == ERROR_FILE_NOT_FOUND) {
            if (!spawned) {
                if (!SpawnServer(sessionId)) return INVALID_HANDLE_VALUE;
                spawned = true;
                // Start the startup allowance now; time already spent queueing
                // behind another client must not count against the new server
                spawnDeadline = GetTickCount64() + CLIENT_SPAWN_DEADLINE_MS;
            }
            if (GetTickCount64() > spawnDeadline) return INVALID_HANDLE_VALUE;
            Sleep(PIPE_CONNECT_RETRY_MS);
            continue;
        }
        return INVALID_HANDLE_VALUE;
    }
}

// Hand the audio to the session's linger server and wait for its playback result
//
// Retries only while the request is unacknowledged, which covers a server that
// shuts down between connect and hand-off (linger expiry, device switch). Once
// the ack arrives the server owns the audio, so a later failure must not retry
// or the same sound would play twice.
static int RunClient(const wchar_t* sessionId, const std::vector<BYTE>& inputData) {
    if (inputData.size() > PIPE_MAX_DATA) {
        PrintError("Input too large for session playback");
        return ERR_DECODE_FAILED;
    }

    PipeRequestHeader header = {};
    memcpy(header.magic, PIPE_MAGIC, sizeof(PIPE_MAGIC));
    header.version  = PIPE_PROTO_VERSION;
    header.dataSize = inputData.size();

    ULONGLONG queueDeadline = GetTickCount64() + CLIENT_QUEUE_DEADLINE_MS;

    for (int attempt = 0; attempt < 2; attempt++) {
        HANDLE pipe = ConnectToServer(sessionId, queueDeadline);
        if (pipe == INVALID_HANDLE_VALUE) continue;

        BYTE ack = 0;
        bool accepted = PipeWriteAll(pipe, &header, sizeof(header), PIPE_IO_TIMEOUT_MS)
                     && PipeWriteAll(pipe, inputData.data(), inputData.size(), PIPE_IO_TIMEOUT_MS)
                     && PipeReadExact(pipe, &ack, sizeof(ack), PIPE_IO_TIMEOUT_MS)
                     && ack == PIPE_ACK_BYTE;
        if (!accepted) {
            CloseHandle(pipe);
            continue;
        }

        PipeResponse response = {};
        bool ok = PipeReadExact(pipe, &response, sizeof(response), CLIENT_RESPONSE_TIMEOUT_MS);
        CloseHandle(pipe);

        if (!ok) {
            // The audio was accepted, so it may already be playing
            PrintError("Lost contact with linger server during playback");
            return ERR_PLAYBACK_FAILED;
        }

        // Surface the server-side failure cause; the server has no way to
        // reach this process's stderr
        if (response.exitCode == ERR_DECODE_FAILED) PrintError("Failed to decode audio");
        else if (response.exitCode != EXIT_SUCCESS) PrintError("Failed to play audio");
        return response.exitCode;
    }

    PrintError("Failed to communicate with linger server");
    return ERR_PLAYBACK_FAILED;
}

int wmain(int argc, wchar_t* argv[]) {
    const wchar_t* sessionId = nullptr;
    const wchar_t* serveId   = nullptr;
    const wchar_t* pathArg   = nullptr;
    bool argsValid = true;

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--session") == 0) {
            if (i + 1 >= argc || sessionId) { argsValid = false; break; }
            sessionId = argv[++i];
        }
        else if (wcscmp(argv[i], L"--serve") == 0) {
            if (i + 1 >= argc || serveId) { argsValid = false; break; }
            serveId = argv[++i];
        }
        else if (!pathArg) {
            pathArg = argv[i];
        }
        else {
            argsValid = false;
            break;
        }
    }

    if (!argsValid || (sessionId && serveId)) {
        PrintError("Invalid arguments");
        std::cerr << "Usage: minply.exe [--session <id>] [audio file path | -]" << std::endl;
        return ERR_INVALID_ARGS;
    }

    // Server mode is an internal entry point spawned by the client
    if (serveId) {
        if (!IsValidSessionId(serveId)) {
            PrintError("Invalid session id");
            return ERR_INVALID_ARGS;
        }
        return RunServer(serveId);
    }

    if (sessionId && !IsValidSessionId(sessionId)) {
        PrintError("Invalid session id");
        return ERR_INVALID_ARGS;
    }

    // Load audio data into buffer
    //
    // Argument resolution:
    //   - no path argument    : read from stdin if piped, else exit silently
    //   - path argument "-"   : read from stdin (error if empty)
    //   - path argument       : read from file
    std::vector<BYTE> inputData;
    if (!pathArg) {
        if (!ReadAllStdin(inputData)) {
            return EXIT_SUCCESS;
        }
    }
    else if (wcscmp(pathArg, L"-") == 0) {
        if (!ReadAllStdin(inputData)) {
            PrintError("No input data on stdin");
            return ERR_FILE_NOT_FOUND;
        }
    }
    else {
        const wchar_t* filePath = pathArg;
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

    // Session mode delegates playback to the linger server, which loads its own
    // configuration; reading it here as well would only duplicate any warnings
    if (sessionId) {
        return RunClient(sessionId, inputData);
    }

    AppConfig config = LoadConfig();

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
    // Playback writes decoded float frames straight into the WASAPI buffer, so
    // any other layout would read or write past its end
    else if (!IsFloat32MixFormat(mixFormat)) {
        PrintError("Unsupported device mix format");
        exitCode = ERR_WASAPI_INIT;
        MFShutdown();
        CoTaskMemFree(mixFormat);
    }
    else {
        std::vector<float> decodedData;
        bool decoded = DecodeInput(inputData.data(), inputData.size(), mixFormat, decodedData);

        MFShutdown();

        if (!decoded) {
            PrintError("Failed to decode audio");
            exitCode = ERR_DECODE_FAILED;
        }
        else {
            PostProcess(decodedData, mixFormat, config);

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
