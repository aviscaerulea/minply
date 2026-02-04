# minply

**Bluetooth レシーバの先頭欠損を自動補償する、超軽量 CLI オーディオプレイヤー**

通知音などの短い音声を高速軽量に再生するための Windows 用オーディオプレイヤー。

## 特徴

- 単一実行ファイル（約 211KB）、ランタイム依存なし
- MP3, WAV, AAC, FLAC, WMA などの形式に対応（Windows Media Foundation 使用）
- BLE レシーバの遅延補償のための自動無音再生（0.7 秒）
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
- [Task](https://taskfile.dev/)（オプション、`task build` を使う場合）

## ビルド方法

```powershell
# Taskfile を使う場合
task build

# PowerShell で直接実行する場合
pwsh -ExecutionPolicy Bypass -File build.ps1
```

## 動作の仕組み

1. WASAPI `GetMixFormat` でデフォルトオーディオデバイスのネイティブ形式を取得
2. Media Foundation でオーディオファイルをデコードし、デバイスの形式にリサンプリング
3. BLE レシーバのウェイクアップ遅延を補償するため、同一セッション内で 0.7 秒の無音を先行再生
4. セッション起動時のノイズを無音で吸収した後、続けてオーディオファイルを再生

## ライセンス

MIT
