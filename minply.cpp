/*
 * minply - Minimal Audio Player
 *
 * Lightweight and fast audio player with BLE receiver lag compensation
 *
 * Usage:
 *   minply.exe <audio file path>
 *
 * Features:
 *   - Instantly plays MP3, WAV, AAC, FLAC and other audio files
 *   - Plays 0.7 seconds of silence before audio playback (BLE lag compensation)
 *   - Exits immediately after playback completes
 *
 * Dependencies:
 *   - Windows Media Foundation: Audio decoder
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

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

// Error codes
#define EXIT_SUCCESS          0
#define ERR_INVALID_ARGS      1
#define ERR_FILE_NOT_FOUND    2
#define ERR_DECODE_FAILED     3
#define ERR_WASAPI_INIT       4
#define ERR_PLAYBACK_FAILED   5

// Constants
constexpr float SILENCE_DURATION = 0.7f;  // Silence duration in seconds (BLE lag compensation)
constexpr DWORD BUFFER_WAIT_MS = 100;      // Buffer wait time in milliseconds

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

// Generate silence buffer
std::vector<float> GenerateSilence(UINT32 sampleRate, UINT32 channels) {
    size_t silenceSamples = static_cast<size_t>(sampleRate * SILENCE_DURATION * channels);
    return std::vector<float>(silenceSamples, 0.0f);
}

// Play audio using WASAPI
bool PlayAudio(const std::vector<float>& audioData, const WAVEFORMATEX* mixFormat,
               const std::vector<float>& leadIn = {}) {
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

        // Play lead-in data (silence for BLE wake-up), then main audio data
        const std::vector<float>* sources[] = { &leadIn, &audioData };
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
        Sleep(150);

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
        if (!DecodeAudioFile(filePath, decodedData, mixFormat->nSamplesPerSec, mixFormat->nChannels)) {
            PrintError("Failed to decode audio file");
            exitCode = ERR_DECODE_FAILED;
        } else {
            // MF is no longer needed
            MFShutdown();

            // Generate silence buffer
            std::vector<float> silenceData = GenerateSilence(mixFormat->nSamplesPerSec, mixFormat->nChannels);

            // Play with lead-in silence to absorb WASAPI session startup noise and BLE wake-up delay
            if (!PlayAudio(decodedData, mixFormat, silenceData)) {
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
