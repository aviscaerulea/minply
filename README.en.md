# minply

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/minply)](https://github.com/aviscaerulea/minply/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/minply)](LICENSE)
[![Build](https://github.com/aviscaerulea/minply/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/minply/actions/workflows/release.yml)

An ultra-lightweight CLI audio player for Windows, made for notification sounds and text-to-speech output.

Bluetooth receivers enter a power-saving state after a period of silence, which cuts off the beginning of the next sound they play. minply prevents this clipping by continuously playing an inaudible high-frequency tone before and after the audio. It is a single executable of roughly 690KB and needs no additional runtime.

## Features

- Clipping prevention: keeps the receiver awake with an inaudible tone so the start of a sound is never cut off
- Session linger: removes the delay that otherwise occurs on every playback in a series
- Broad format support: plays Opus, MP3, WAV, AAC, FLAC, WMA and more
- Standard input playback: audio data can be piped in instead of passing a file path
- Single executable: no installation required, just drop it in place
- Immediate exit: plays without showing a window and exits as soon as it finishes
- Mute conditions: stays silent while specified apps are running or while a specific site is open in the browser

## Installation

### Requirements

- Windows 10 version 1607 or later (x64)
- No additional software required (all dependencies are built into Windows)

### Steps

#### From the release ZIP

Download `minply-<version>-x64.zip` from the [releases page](https://github.com/aviscaerulea/minply/releases/latest). Extract it and place `minply.exe` in any folder you like. Placing the bundled `minply.toml` in the same folder lets you adjust the behavior.

#### From Scoop

If you use [Scoop](https://scoop.sh/), add the bucket and install.

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install minply
```

## Usage

```
minply.exe [--session <id>] [audio file | -]
```

```powershell
# Play a file
minply.exe notification.mp3

# Capture the error output
minply.exe notfound.mp3 2>&1 | Out-File error.log
```

### Playing from standard input

If you omit the argument or pass `-`, minply reads audio data from standard input and plays it.

```cmd
:: Piping from cmd.exe (the argument may be omitted)
type alert.mp3 | minply.exe
```

```powershell
# Piping from PowerShell 7 or later
Get-Content -AsByteStream -Raw alert.mp3 | minply.exe
```

> [!IMPORTANT]
> PowerShell's `Get-Content` reads as text by default, which corrupts binary data. Always specify `-AsByteStream` when piping (PowerShell 7 or later).

### Speeding up consecutive playback

Adding `--session <id>` keeps the process resident for a while after playback, continuously emitting the inaudible tone. The receiver stays awake, so the next playback with the same id starts without any delay. This helps when playing text-to-speech output in succession.

```powershell
# Example of passing a session ID
minply.exe --session $env:CLAUDE_SESSION_ID speech.wav
```

- First run: starts the resident process, wakes the receiver, then plays
- Subsequent runs: plays without delay as long as the id is the same
- The resident process exits automatically once a set time has passed since the last playback

Playback requests sharing an id are handled one at a time in order, while different ids play simultaneously and independently. Use the same id when you do not want consecutive sounds to overlap.

An id may contain alphanumeric characters plus `.`, `_` and `-`, and must be 1 to 64 characters long.

### Exit codes

| Code | Description |
| --- | --- |
| 0 | Success |
| 1 | Invalid argument |
| 2 | File not found, unreadable, or empty |
| 3 | Decoding failed |
| 4 | Audio device initialization failed |
| 5 | Playback failed (including failure to start or reach the resident process) |

### Log

Errors and configuration file warnings are written to standard error and also appended, one line each, to `%LOCALAPPDATA%\minply\logs\YYYY-MM-DD.log`; successful playbacks are not logged. Standard error is not visible when minply runs from a hook or while a `--session` process stays resident, so use this log to find out why a sound did not play.

## Configuration

Placing `minply.toml` in the same folder as the executable lets you adjust the behavior. Without it, minply runs with the default values. If you also place `minply.local.toml`, it is loaded afterwards and overrides the settings. Use it for settings specific to your own environment.

The meaning and default value of each item are documented in the comments inside `minply.toml`. The main items are as follows.

| Section | Item | Description |
| --- | --- | --- |
| `guard` | `enabled` | Whether to prevent clipping with the inaudible tone |
| `guard` | `lead_in_duration` | Seconds to play before the audio |
| `guard` | `lead_out_duration` | Seconds to play after the audio |
| `loudness` | `enabled` | Whether to even out volume differences between sources |
| `loudness` | `target` | The loudness to normalize to |
| `linger` | `duration` | Seconds to stay resident when `--session` is given |
| `mute` | `processes` | Apps (exe names) that suppress playback while running |
| `mute.window` | `process`, `title` | Suppress playback when a window title of the app matches the regular expression |

Values outside the allowed range, and invalid values, fall back to the default after a warning is written to standard error and the log.

## Limitations

- Output is fixed to the Windows default audio device. The playback device cannot be selected
- Opus support is limited to mono and stereo (channel mapping family 0)
- Configuration files are not reloaded while a `--session` process is resident. Changes take effect from the first playback after it exits
- Switching the audio device does not terminate the resident process right away. The next invocation terminates it and starts a new one for the new device
- A browser window title shows only the tab currently displayed in that window, so a site open in a background tab does not match
- When a mute condition holds, minply exits with code 0 without playing, even if the file does not exist, and records the reason in the log

## Build

The vcpkg dependencies are installed automatically when `task build` runs.

```powershell
# Standard build (includes automatic installation of dependencies)
task build

# Building directly from PowerShell (requires vcpkg install beforehand)
pwsh -ExecutionPolicy Bypass -File build.ps1
```

The language is C++17 (MSVC). Decoding uses Windows Media Foundation and audio output uses WASAPI. The following libraries are statically linked.

- [libopus](https://opus-codec.org/): Opus codec
- [libogg](https://xiph.org/ogg/): Ogg container
- [libebur128](https://github.com/jiixyj/libebur128): EBU R128 loudness measurement
