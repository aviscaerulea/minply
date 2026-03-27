# minply

**Bluetooth レシーバの先頭欠損を自動補償する、超軽量 CLI オーディオプレイヤー**

通知音などの短い音声を高速軽量に再生するための Windows 用オーディオプレイヤー。

## 特徴

- 単一実行ファイル（約 408KB）、ランタイム依存なし
- Opus, MP3, WAV, AAC, FLAC, WMA などの形式に対応
- WAV ファイルは可能な場合リサンプリングなしで直接再生（音質劣化なし）
- Opus ファイル（.opus, .ogg）の高品質再生対応
- BLE レシーバの省電力モード抑止のための不可聴ガードトーン再生（冒頭 1.5 秒・末尾 2.0 秒）
- バックグラウンド実行（コンソールウィンドウなし）
- WASAPI 共有モードでデフォルトオーディオデバイスに再生
- 再生完了後に即座に終了

## 使い方

```
minply.exe <オーディオファイル>
```

```powershell
# MP3 ファイルを再生
minply.exe notification.mp3

# WAV ファイルを再生
minply.exe alert.wav

# 他のコマンドと連携
minply.exe done.mp3 && echo "再生完了"

# エラー出力をキャプチャ
minply.exe notfound.mp3 2>&1 | Out-File error.log
```

## 終了コード

| コード | 説明 |
|------|------|
| 0 | 成功 |
| 1 | 引数が無効 |
| 2 | ファイルが見つからない |
| 3 | デコード失敗 |
| 4 | オーディオデバイスの初期化失敗 |
| 5 | 再生失敗 |

## 動作要件

### 実行

- Windows 10 version 1607 以降（x64）
- 追加のソフトウェアは不要（全ての依存関係は Windows に組み込み済み）

### ビルド

- [Visual Studio](https://visualstudio.microsoft.com/) 2019 以降（C++ デスクトップ開発ワークロード）
- [PowerShell 7+](https://github.com/PowerShell/PowerShell)（pwsh）
- [vcpkg](https://github.com/microsoft/vcpkg)（Opus と Ogg ライブラリ用）
- [Task](https://taskfile.dev/)（オプション、`task build` を使う場合）

## ビルド方法

```powershell
# vcpkg で依存ライブラリをインストール（初回のみ）
vcpkg install opus:x64-windows-static libogg:x64-windows-static

# 通常ビルド
task build

# リリースビルド（クリーンビルド → exe を zip に圧縮）
task release

# PowerShell で直接実行する場合
pwsh -ExecutionPolicy Bypass -File build.ps1
```

## 動作の仕組み

1. WASAPI `GetMixFormat` でデフォルトオーディオデバイスのネイティブ形式を取得
2. デコード優先順位：
   - WAV ファイル：サンプルレートとチャンネル数が一致すれば直接読み込み（音質劣化なし）
   - Opus ファイル：libopus でデコード（.opus, .ogg 対応）
   - その他の形式：Media Foundation でデコード（MP3, AAC, FLAC, WMA など）
3. BLE レシーバの省電力モード移行を防ぐため、冒頭 1.5 秒・末尾 2.0 秒に不可聴の 19kHz ガードトーンを再生
4. ガードトーンでレシーバを起床・維持し、続けてオーディオファイルを再生

## ライセンス

MIT
