# minply

**Minimal audio player for Windows with Bluetooth lag compensation.**

Bluetooth レシーバの先頭欠損を自動補償する、軽量コマンドラインオーディオプレイヤー。

## Features

- Single-file executable (~211KB), no runtime dependencies
- Supports MP3, WAV, AAC, FLAC, WMA and other formats (via Windows Media Foundation)
- Automatic silence padding at the beginning for BLE receiver lag compensation
- Plays to the default audio device using WASAPI shared mode
- Exits immediately after playback completes

## Usage

```
minply.exe <audio file>
```

```powershell
# Play an MP3 file
minply.exe notification.mp3

# Play a WAV file
minply.exe alert.wav

# Chain with other commands
minply.exe done.mp3 && echo "Playback finished"
```

## Exit Codes

| Code | Description |
|------|-------------|
| 0 | Success |
| 1 | Invalid arguments |
| 2 | File not found |
| 3 | Decode failed |
| 4 | Audio device initialization failed |
| 5 | Playback failed |

## Build

Requires Visual Studio 2019 or later with C++17 support.

```powershell
# Using Taskfile
task build

# Using PowerShell directly
pwsh -ExecutionPolicy Bypass -File build.ps1
```

## How It Works

1. Queries the default audio device's native format via WASAPI `GetMixFormat`
2. Decodes the audio file using Media Foundation, resampling to the device's format
3. Prepends 0.4 seconds of silence to compensate for BLE receiver wake-up latency
4. Streams the audio data to the device using WASAPI event-driven shared mode

## Requirements

- Windows 10 version 1607 or later

## License

MIT
