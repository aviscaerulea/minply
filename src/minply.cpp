/*
 * minply - Minimal Audio Player
 *
 * Lightweight and fast audio player with BLE receiver lag compensation
 *
 * Usage:
 *   minply.exe <audio file path>
 *
 * Features:
 *   - Instantly plays MP3, WAV, AAC, FLAC, Opus and other audio files
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
#include <iostream>
#include <vector>
#include <ogg/ogg.h>
#include <opus/opus.h>
#include <ebur128.h>
#include <cmath>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
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

constexpr float LOUDNESS_TARGET = -6.0f;         // Target integrated loudness in LUFS (optimized for notification sounds)
constexpr float LOUDNESS_PEAK_CEILING = 0.891f;  // True peak ceiling (-1dBFS); prevents clipping after loudness gain
constexpr float LOUDNESS_MIN_PEAK = 1e-6f;       // Minimum peak threshold; skip normalization for near-silence

constexpr float FADE_DURATION = 0.005f;    // Fade in/out duration in seconds (click noise reduction)
constexpr DWORD BUFFER_WAIT_MS = 100;      // Buffer wait time in milliseconds
constexpr DWORD DRAIN_WAIT_MS = 300;       // Wait time for device buffer drain in milliseconds

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

// Read WAV file directly without Media Foundation (bypass resampling for matching formats)
bool TryReadWavDirect(const wchar_t* filePath, std::vector<float>& audioData,
                      UINT32 targetSampleRate, UINT32 targetChannels) {
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    auto closeFile = [&]() { CloseHandle(hFile); };

    // Read RIFF/WAVE header
    char header[12];
    DWORD bytesRead;
    if (!ReadFile(hFile, header, 12, &bytesRead, nullptr) || bytesRead != 12) {
        closeFile();
        return false;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        closeFile();
        return false;
    }

    // Find fmt chunk
    WAVEFORMATEXTENSIBLE fmt = {};
    bool fmtFound = false;
    while (true) {
        char chunkId[4];
        DWORD chunkSize;
        if (!ReadFile(hFile, chunkId, 4, &bytesRead, nullptr) || bytesRead != 4) break;
        if (!ReadFile(hFile, &chunkSize, 4, &bytesRead, nullptr) || bytesRead != 4) break;

        if (memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) break;
            BYTE fmtBuf[40] = {};
            DWORD fmtReadSize = (std::min)(chunkSize, static_cast<DWORD>(40));
            if (!ReadFile(hFile, fmtBuf, fmtReadSize, &bytesRead, nullptr)) break;
            memcpy(&fmt, fmtBuf, fmtReadSize);
            fmtFound = true;
            if (chunkSize > fmtReadSize) {
                SetFilePointer(hFile, chunkSize - fmtReadSize, nullptr, FILE_CURRENT);
            }
            break;
        }
        SetFilePointer(hFile, chunkSize, nullptr, FILE_CURRENT);
    }

    if (!fmtFound) {
        closeFile();
        return false;
    }

    // Normalize format tag
    WORD actualFormatTag = fmt.Format.wFormatTag;
    if (actualFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt.Format.cbSize >= 22) {
        actualFormatTag = *reinterpret_cast<WORD*>(&fmt.SubFormat);
    }

    // Check format compatibility
    if (actualFormatTag != WAVE_FORMAT_PCM && actualFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
        closeFile();
        return false;
    }
    if (fmt.Format.nSamplesPerSec != targetSampleRate) {
        closeFile();
        return false;
    }

    // Find data chunk
    DWORD dataSize = 0;
    bool dataFound = false;
    SetFilePointer(hFile, 12, nullptr, FILE_BEGIN);
    while (true) {
        char chunkId[4];
        DWORD chunkSize;
        if (!ReadFile(hFile, chunkId, 4, &bytesRead, nullptr) || bytesRead != 4) break;
        if (!ReadFile(hFile, &chunkSize, 4, &bytesRead, nullptr) || bytesRead != 4) break;

        if (memcmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            dataFound = true;
            break;
        }
        SetFilePointer(hFile, chunkSize, nullptr, FILE_CURRENT);
    }

    if (!dataFound || dataSize == 0) {
        closeFile();
        return false;
    }

    // Read and convert PCM data to float32
    UINT32 bytesPerSample = fmt.Format.wBitsPerSample / 8;
    UINT32 totalSamples = dataSize / bytesPerSample;
    audioData.resize(totalSamples);

    if (actualFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt.Format.wBitsPerSample == 32) {
        // float32 WAV - direct read
        if (!ReadFile(hFile, audioData.data(), dataSize, &bytesRead, nullptr) || bytesRead != dataSize) {
            closeFile();
            return false;
        }
    } else if (actualFormatTag == WAVE_FORMAT_PCM) {
        // int PCM - convert to float32
        std::vector<BYTE> rawData(dataSize);
        if (!ReadFile(hFile, rawData.data(), dataSize, &bytesRead, nullptr) || bytesRead != dataSize) {
            closeFile();
            return false;
        }

        if (fmt.Format.wBitsPerSample == 16) {
            // int16 -> float32
            const int16_t* samples = reinterpret_cast<const int16_t*>(rawData.data());
            for (UINT32 i = 0; i < totalSamples; i++) {
                audioData[i] = static_cast<float>(samples[i]) / 32768.0f;
            }
        } else if (fmt.Format.wBitsPerSample == 24) {
            // int24 -> float32
            for (UINT32 i = 0; i < totalSamples; i++) {
                int32_t sample = (rawData[i * 3] << 8) | (rawData[i * 3 + 1] << 16) | (rawData[i * 3 + 2] << 24);
                sample >>= 8; // sign extend
                audioData[i] = static_cast<float>(sample) / 8388608.0f;
            }
        } else if (fmt.Format.wBitsPerSample == 32) {
            // int32 -> float32
            const int32_t* samples = reinterpret_cast<const int32_t*>(rawData.data());
            for (UINT32 i = 0; i < totalSamples; i++) {
                audioData[i] = static_cast<float>(samples[i]) / 2147483648.0f;
            }
        } else {
            closeFile();
            return false;
        }
    } else {
        closeFile();
        return false;
    }

    closeFile();

    // Channel conversion (e.g. mono -> stereo)
    if (fmt.Format.nChannels != targetChannels) {
        audioData = ConvertFormat(audioData, targetSampleRate, fmt.Format.nChannels,
                                  targetSampleRate, targetChannels);
    }

    return true;
}

// Check if file is Opus format
bool IsOpusFile(const wchar_t* filePath) {
    const wchar_t* ext = wcsrchr(filePath, L'.');
    if (!ext) return false;
    return (_wcsicmp(ext, L".opus") == 0 || _wcsicmp(ext, L".ogg") == 0);
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

// Decode Opus file (.opus and .ogg Opus)
bool TryDecodeOpusFile(const wchar_t* filePath, std::vector<float>& audioData,
                       UINT32 targetSampleRate, UINT32 targetChannels) {
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    auto closeFile = [&]() { CloseHandle(hFile); };

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

    const size_t CHUNK_SIZE = 4096;
    bool success = false;

    // Opus stream structure: packet 1 = OpusHead, packet 2 = OpusTags, packet 3+ = audio data
    while (true) {
        char* buffer = ogg_sync_buffer(&oy, CHUNK_SIZE);
        if (!buffer) break;
        DWORD bytesRead;
        if (!ReadFile(hFile, buffer, CHUNK_SIZE, &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }
        ogg_sync_wrote(&oy, bytesRead);

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
                            closeFile();
                            if (streamInitialized) ogg_stream_clear(&os);
                            ogg_sync_clear(&oy);
                            return false;
                        }
                    } else {
                        // Not an Opus file
                        closeFile();
                        if (decoder) opus_decoder_destroy(decoder);
                        if (streamInitialized) ogg_stream_clear(&os);
                        ogg_sync_clear(&oy);
                        return false;
                    }
                } else if (packetCount == 2) {
                    // OpusTags packet (metadata, skip)
                    continue;
                } else {
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
    }

    closeFile();

    if (decoder) opus_decoder_destroy(decoder);
    if (streamInitialized) ogg_stream_clear(&os);
    ogg_sync_clear(&oy);

    if (decodedFloat.empty()) return false;

    // Format conversion
    audioData = ConvertFormat(decodedFloat, opusSampleRate, opusChannels,
                              targetSampleRate, targetChannels);

    return true;
}

// Decode audio file using Media Foundation
bool DecodeAudioFile(const wchar_t* filePath, std::vector<float>& decodedData,
                     UINT32 targetSampleRate, UINT32 targetChannels) {
    HRESULT hr;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* mediaType = nullptr;
    bool success = false;

    do {
        // Create source reader
        hr = MFCreateSourceReaderFromURL(filePath, nullptr, &reader);
        if (FAILED(hr)) {
            PrintError("Failed to open audio file");
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
                    BYTE* data = nullptr;
                    DWORD dataLen = 0;

                    hr = buffer->Lock(&data, nullptr, &dataLen);
                    if (SUCCEEDED(hr)) {
                        size_t sampleCount = dataLen / sizeof(float);
                        size_t oldSize = decodedData.size();
                        decodedData.resize(oldSize + sampleCount);
                        memcpy(&decodedData[oldSize], data, dataLen);
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

    return success;
}

// Generate inaudible 19kHz sine wave buffer for BLE anti-clipping
// BLE devices enter power-saving mode on digital silence, causing audio clipping.
// A 19kHz tone at ~-60dB is inaudible to adults but keeps the BLE codec active.
std::vector<float> GenerateBleGuard(UINT32 sampleRate, UINT32 channels, float duration) {
    size_t totalFrames = static_cast<size_t>(sampleRate * duration);
    std::vector<float> buffer(totalFrames * channels);

    for (size_t i = 0; i < totalFrames; i++) {
        float t = static_cast<float>(i) / sampleRate;
        float sample = BLE_GUARD_AMP * sinf(TWO_PI * BLE_GUARD_FREQ * t);
        for (UINT32 ch = 0; ch < channels; ch++) {
            buffer[i * channels + ch] = sample;
        }
    }

    return buffer;
}

// Normalize audio to LOUDNESS_TARGET using EBU R128 integrated loudness (ITU-R BS.1770-4)
//
// Measures integrated loudness via libebur128, computes gain to reach LOUDNESS_TARGET,
// then clamps gain if true peak would exceed LOUDNESS_PEAK_CEILING.
void NormalizeLoudness(std::vector<float>& audioData, UINT32 sampleRate, UINT32 channels) {
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

    float gain = static_cast<float>(pow(10.0, (LOUDNESS_TARGET - loudness) / 20.0));

    if (peak * gain > LOUDNESS_PEAK_CEILING) {
        gain = LOUDNESS_PEAK_CEILING / peak;
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

int wmain(int argc, wchar_t* argv[]) {
    // Check arguments
    if (argc != 2) {
        PrintError("Invalid arguments");
        std::cerr << "Usage: minply.exe <audio file path>" << std::endl;
        return ERR_INVALID_ARGS;
    }

    const wchar_t* filePath = argv[1];

    // Check if file exists
    DWORD fileAttr = GetFileAttributesW(filePath);
    if (fileAttr == INVALID_FILE_ATTRIBUTES) {
        PrintError("File not found");
        return ERR_FILE_NOT_FOUND;
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
    } else {
        // Decode audio file
        std::vector<float> decodedData;
        // Try WAV direct read (bypass MF resampling for matching formats)
        bool decoded = TryReadWavDirect(filePath, decodedData,
                                        mixFormat->nSamplesPerSec, mixFormat->nChannels);
        if (!decoded && IsOpusFile(filePath)) {
            // Try Opus decode
            decoded = TryDecodeOpusFile(filePath, decodedData,
                                        mixFormat->nSamplesPerSec, mixFormat->nChannels);
        }
        if (!decoded) {
            // Fallback to Media Foundation decoder
            decoded = DecodeAudioFile(filePath, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);
        }

        if (!decoded) {
            PrintError("Failed to decode audio file");
            exitCode = ERR_DECODE_FAILED;
        } else {
            // MF is no longer needed
            MFShutdown();

            // Normalize perceived loudness (EBU R128) to unify volume across sources
            NormalizeLoudness(decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);

            // Apply fade to prevent click noise
            ApplyFade(decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels);

            // Generate BLE guard tone buffers
            std::vector<float> leadIn = GenerateBleGuard(
                mixFormat->nSamplesPerSec, mixFormat->nChannels, LEAD_IN_DURATION);
            std::vector<float> leadOut = GenerateBleGuard(
                mixFormat->nSamplesPerSec, mixFormat->nChannels, LEAD_OUT_DURATION);

            // Play with BLE guard tones to prevent BLE device from entering power-saving mode
            if (!PlayAudio(decodedData, mixFormat, leadIn, leadOut)) {
                PrintError("Failed to play audio");
                exitCode = ERR_PLAYBACK_FAILED;
            }
        }

        CoTaskMemFree(mixFormat);
    }

    // Cleanup
    if (exitCode == ERR_WASAPI_INIT) {
        MFShutdown();
    }
    CoUninitialize();

    return exitCode;
}
