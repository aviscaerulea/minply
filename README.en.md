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
- Mute conditions: detects a specified app running or playing sound, a specific site open in the browser, or a microphone or camera in use, and skips playback
- Manual mute: silences playback from the notification area menu, for a set time or with no time limit

## Installation

### Requirements

- Windows 10 version 1607 or later (x64)
- No additional software required (all dependencies are built into Windows)

### Steps

#### From the release ZIP

Download `minply-<version>-x64.zip` from the [releases page](https://github.com/aviscaerulea/minply/releases/latest). Extract it and place `minply.exe` in any folder you like. Placing the bundled `minply.toml` and `minply.local.toml` in the same folder lets you adjust the behavior (see [Configuration](#configuration)).

When updating to a new version, replace only `minply.exe` and `minply.toml`. Overwriting `minply.local.toml` as well erases the settings you wrote.

#### From Scoop

If you use [Scoop](https://scoop.sh/), add the bucket and install.

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install minply
```

#### Uninstalling

The notification area settings and the startup registration stay in the Windows registry. `--uninstall` closes the running tray and deletes the registry settings and the logs of older versions (`%LOCALAPPDATA%\minply`). Before removing minply, run the following command.

```powershell
minply.exe --uninstall
```

The configuration files and the logs (the `logs` folder) sit in the same folder as `minply.exe`, and this command leaves them in place. If you installed from the ZIP, delete the whole folder. With Scoop, `scoop uninstall minply --purge` also removes the configuration files and the logs.

## Usage

```
minply.exe [--session <id>] [audio file | -]
minply.exe --tray
minply.exe --uninstall
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

### Notification area

Starting `minply.exe --tray` keeps minply resident in the notification area. If a tray is already running, it is closed and replaced by the new one. The icon's right-click menu (in Japanese) offers the following.

| Operation | Description |
| --- | --- |
| Manual mute | Silences playback with no time limit, or for a configured time (30 minutes, 60 minutes, 90 minutes, or 12 hours by default) |
| Mute while a microphone or camera is in use | Silences playback while a microphone or camera is in use, such as during a meeting (off by default) |
| Startup registration | Starts the tray automatically at sign-in |
| Open the configuration file, open the log file | Opens each file in its default app |

While playback is held back, the icon shows a red slash, and its direction tells the kind of reason. Hovering over the icon shows why playback is held back.

| Icon mark | Reason |
| --- | --- |
| Slash falling to the right (＼) | A microphone or camera in use, or a mute condition in the configuration file |
| Slash rising to the right (／) | Manual mute |
| X | Both |

Left-clicking the icon unmutes while any manual mute is on, and otherwise mutes with no time limit.

When a playback request arrives, a green dot appears at the bottom-right of the icon for 3 seconds, even when the request is not played because of a mute.

The manual mute is released when the tray exits. The microphone/camera setting and the startup registration persist across tray restarts.

### Exit codes

| Code | Description |
| --- | --- |
| 0 | Success |
| 1 | Invalid argument |
| 2 | File not found, unreadable, or empty |
| 3 | Decoding failed |
| 4 | Audio device initialization failed |
| 5 | Playback failed (including failure to start or reach the resident process, failure to start the tray, and a failed `--uninstall`) |

### Log

Errors and configuration file warnings are written to standard error and also appended, one line each, to `logs\YYYY-MM-DD.log` in the same folder as `minply.exe`; successful playbacks are not logged. Standard error is not visible when minply runs from a hook or while a `--session` process stays resident, so use this log to find out why a sound did not play. Each time the tray starts, old logs are deleted, keeping the last 7 days by default.

## Configuration

Write your settings in `minply.local.toml` in the same folder as the executable. Without the file, or for items you do not write, minply runs with the default values.

`minply.toml` is the reference for every setting and its default value, and its comments explain what each item means and how to write it. Each update replaces it with the new version, so do not edit it directly. minply reads `minply.toml` first and then `minply.local.toml`, and the values in the latter take precedence. With Scoop, `minply.local.toml` is carried over across updates.

```toml
# Example minply.local.toml: lower the volume, and stay silent while Zoom is running or Spotify is playing
[loudness]
target = -20.0

[[mute]]
process = "Zoom.exe"

[[mute]]
process = "Spotify.exe"
playing = true
```

The main items are as follows.

| Section | Item | Description |
| --- | --- | --- |
| `guard` | `enabled` | Whether to prevent clipping with the inaudible tone |
| `guard` | `lead_in_duration` | Seconds to play before the audio |
| `guard` | `lead_out_duration` | Seconds to play after the audio |
| `loudness` | `enabled` | Whether to even out volume differences between sources |
| `loudness` | `target` | The loudness to normalize to |
| `linger` | `duration` | Seconds to stay resident when `--session` is given |
| `log` | `retention_days` | Days of logs kept when the tray starts (0 keeps all) |
| `mute` (repeat `[[mute]]`) | `process`, `title`, `playing` | Suppress playback while the app is running, while its window title matches the regular expression, or while it is playing sound (a rule holds when all the conditions written in it hold) |
| `tray` | `condition_interval` | How often the mute conditions are checked and reflected on the icon (seconds) |
| `tray` | `mute_durations` | Manual mute durations shown in the tray menu |

Values outside the allowed range, and invalid values, fall back to the default after a warning is written to standard error and the log. A `[[mute]]` entry that has an invalid value or no `process`, however, is ignored as a whole with a warning.

## Limitations

- Output is fixed to the Windows default audio device. The playback device cannot be selected
- Opus support is limited to mono and stereo (channel mapping family 0)
- Configuration files are not reloaded while a `--session` process is resident. Changes take effect from the first playback after it exits
- Switching the audio device does not terminate the resident process right away. The next invocation terminates it and starts a new one for the new device
- A browser window title shows only the tab currently displayed in that window, so a site open in a background tab does not match
- The playing-sound condition reads the volume at that moment, so it does not hold while the app is paused or silent between tracks
- The playing-sound condition is judged per app, so in a browser it cannot tell which tab is playing the sound
- The tray settings (`condition_interval`, `mute_durations`) are read only when the tray starts
- The red slash for a mute condition reaches the icon up to `condition_interval` (5 seconds by default) late, both when the condition starts and when it ends
- Detecting microphone and camera use relies on the usage records kept by the Windows privacy feature
- When a mute condition holds, minply exits with code 0 without playing, even if the file does not exist, and records the reason in the log
- `v2.0.0` changed the configuration format: `[mute] processes` and `[[mute.window]]` are no longer read (rewrite them as `[[mute]]`)

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
