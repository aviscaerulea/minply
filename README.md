# minply

BLE レシーバの先頭欠損を防止する、通知音など再生用の超軽量 CLI オーディオプレイヤー。

## 機能

- 単一実行ファイル（約 524KB）、ランタイム依存なし
- Opus, MP3, WAV, AAC, FLAC, WMA などの形式に対応
- WAV ファイルは可能な場合リサンプリングなしで直接再生（音質劣化なし）
- Opus ファイル（.opus, .ogg）の高品質再生対応
- EBU R128 ラウドネスノーマライズ（-16 LUFS）によりソースごとの音量差を統一
- BLE レシーバの省電力モード抑止のための不可聴ガードトーン再生（冒頭 1.2 秒・末尾 1.2 秒）
- バックグラウンド実行（コンソールウィンドウなし）
- WASAPI 共有モードでデフォルトオーディオデバイスに再生
- 再生完了後に即座に終了

## 動作要件

### 実行

- Windows 10 version 1607 以降（x64）
- 追加のソフトウェアは不要（全ての依存関係は Windows に組み込み済み）

## インストール方法

[Scoop](https://scoop.sh/) でインストールできる。

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install minply
```

## 使用方法

```
minply.exe <オーディオファイル>
```

```powershell
# MP3 ファイルを再生
minply.exe notification.mp3

# WAV ファイルを再生
minply.exe alert.wav

# エラー出力をキャプチャ
minply.exe notfound.mp3 2>&1 | Out-File error.log
```

### 終了コード

| コード | 説明 |
|------|------|
| 0 | 成功 |
| 1 | 引数が無効 |
| 2 | ファイルが見つからない |
| 3 | デコード失敗 |
| 4 | オーディオデバイスの初期化失敗 |
| 5 | 再生失敗 |

## 設定ファイル

実行ファイルと同じディレクトリに `minply.toml` を置くことで動作を調整できる。設定ファイルがない場合はデフォルト値で動作する。

`minply.local.toml` を用意すると `minply.toml` の後に読み込まれ、設定を上書きできる（個人環境専用の設定に使用する）。

```toml
[guard]
# ガードトーンの有効/無効（デフォルト: true）
enabled = true
# 周波数 Hz（デフォルト: 19000.0）
frequency = 19000.0
# 振幅、約 -60dB（デフォルト: 0.001）
amplitude = 0.001
# リードイン秒数（デフォルト: 1.2）
lead_in_duration = 1.2
# リードアウト秒数（デフォルト: 1.2）
lead_out_duration = 1.2

[loudness]
# ラウドネスノーマライズの有効/無効（デフォルト: true）
enabled = true
# 目標ラウドネス LUFS（デフォルト: -16.0）
target = -16.0
# トゥルーピーク上限（デフォルト: 0.891 = -1dBFS）
peak_ceiling = 0.891
```

## 技術仕様

- 言語：C++17（MSVC）
- 依存ライブラリ（静的リンク、vcpkg 管理）
  - [libopus](https://opus-codec.org/)：Opus コーデック
  - [libogg](https://xiph.org/ogg/)：Ogg コンテナ
  - [libebur128](https://github.com/jiixyj/libebur128)：EBU R128 ラウドネス測定
- Windows API：Windows Media Foundation（デコード）、WASAPI（オーディオ出力）

## ビルド方法

vcpkg の依存ライブラリは `task build` 実行時に自動インストールされる。

```powershell
# 通常ビルド（依存ライブラリの自動インストール含む）
task build

# PowerShell で直接ビルドする場合（事前に vcpkg install が必要）
pwsh -ExecutionPolicy Bypass -File build.ps1
```

