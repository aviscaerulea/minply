# minply 技術仕様書

## 概要

minply は Bluetooth Low Energy (BLE) レシーバの音声遅延と先頭欠損を補償する超軽量 CLI オーディオプレイヤー。Windows Media Foundation (WMF), libopus, WASAPI を使用し、単一実行ファイル（約 408KB）として動作する。

## 技術スタック

### API とライブラリ

- **Windows Media Foundation (WMF)**: オーディオデコード
  - `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib`
  - MP3, AAC, FLAC, WMA など多様な形式に対応
- **libopus**: Opus オーディオコーデックのデコード
  - `opus.lib` (vcpkg: opus:x64-windows-static)
  - Opus 形式（.opus, .ogg）に対応
- **libogg**: Ogg コンテナフォーマットのパース
  - `ogg.lib` (vcpkg: libogg:x64-windows-static)
- **WASAPI (Windows Audio Session API)**: オーディオ出力
  - イベント駆動共有モード
  - デフォルトオーディオデバイスへの再生
- **COM (Component Object Model)**: Windows API の基盤

### ビルド環境

- 言語: C++
- コンパイラ: Visual Studio 2019 以降（C++ デスクトップ開発ワークロード）
- ビルドツール: PowerShell 7+, Task
- パッケージマネージャ: vcpkg
- ターゲット: Windows 10 version 1607 以降（x64）

## アーキテクチャ

### 実行フロー

```
1. コマンドライン引数検証
2. ファイル存在確認
3. COM 初期化
4. Media Foundation 初期化
5. WASAPI デバイスのミックスフォーマット取得
6. オーディオファイルデコード
   ├─ WAV 直接読み込み試行（TryReadWavDirect）
   ├─ Opus デコード試行（TryDecodeOpusFile）
   └─ 失敗時は MF デコード（DecodeAudioFile）
7. フェードイン/アウト適用（ApplyFade）
8. BLE ガードトーン生成（GenerateBleGuard）
9. WASAPI セッション開始＆再生（PlayAudio）
   ├─ リードイン 1.2 秒（19kHz ガードトーン）
   ├─ オーディオデータ再生
   └─ リードアウト 1.2 秒（19kHz ガードトーン）
10. バッファドレイン待機
11. リソース解放＆終了
```

### 定数

```cpp
constexpr float LEAD_IN_DURATION = 1.2f;    // リードイン時間（秒）：BLE 起床（約 700ms）+ WASAPI セッション起動ノイズ吸収マージン
constexpr float LEAD_OUT_DURATION = 1.2f;   // リードアウト時間（秒）：音声末尾が BLE/SBC コーデック遅延パイプラインを通過するまで待機
constexpr float BLE_GUARD_FREQ = 19000.0f;  // ガードトーン周波数（Hz）
constexpr float BLE_GUARD_AMP = 0.001f;     // ガードトーン振幅（約 -60dB）
constexpr float FADE_DURATION = 0.005f;     // フェード時間（秒）
constexpr DWORD BUFFER_WAIT_MS = 100;       // バッファ待機時間（ミリ秒）
constexpr DWORD DRAIN_WAIT_MS = 300;        // ドレイン待機時間（ミリ秒）
```

### 終了コード

| コード | 定数名 | 説明 |
|------|--------|------|
| 0 | EXIT_SUCCESS | 成功 |
| 1 | ERR_INVALID_ARGS | 引数が無効 |
| 2 | ERR_FILE_NOT_FOUND | ファイルが見つからない |
| 3 | ERR_DECODE_FAILED | デコード失敗 |
| 4 | ERR_WASAPI_INIT | オーディオデバイスの初期化失敗 |
| 5 | ERR_PLAYBACK_FAILED | 再生失敗 |

## BLE クリッピング抑止機構

### 目的

BLE オーディオレシーバは電力節約のため、音声信号が途切れるとスリープモードに入る。
完全な無音（デジタルサイレンス）はスリープ移行の原因となり、本編冒頭・末尾の音声が欠損する。

### 実装

1. **単一 WASAPI セッションでの 19kHz ガードトーン再生**
   - セッションを分割しない（分割すると再スリープする）
   - リードイン（1.2 秒）: BLE レシーバを起床し、WASAPI セッション起動ノイズを吸収
   - 本編オーディオ: デジタルサイレンスなしで連続再生
   - リードアウト（1.2 秒）: 本編末尾での BLE スリープを防止

2. **ガードトーン仕様**
   - 周波数: 19kHz（成人の可聴域外）
   - 振幅: 0.001（約 -60dB）
   - 効果: BLE コーデックは非ゼロデータとして認識し省電力モードに入らない

3. **実装詳細**
   ```cpp
   const std::vector<float>* sources[] = { &leadIn, &audioData, &leadOut };
   for (const auto* source : sources) {
       // 各データソースを順次再生
   }
   ```

4. **効果**
   - ガードトーンでレシーバが起床し、本編音声は先頭から欠損なく再生される
   - 本編末尾もガードトーンで保護され、再生終了時の尻切れを防止

## クリックノイズ軽減機構

### 原因

1. **波形の不連続性**
   - 無音（0.0f）から本編の最初のサンプル値への急峻な変化
   - 本編の最後のサンプル値から無音（0.0f）への急峻な変化
   - 不連続点で可聴域のクリック音（プチッ音）が発生

2. **バッファドレイン不足**
   - SBC コーデックのエンコード遅延（約 200ms）
   - バッファに残留データがある状態でセッションを閉じるとノイズ発生

### 実装

1. **フェードイン/アウト処理（5ms）**
   ```cpp
   void ApplyFade(std::vector<float>& audioData, UINT32 sampleRate, UINT32 channels)
   ```
   - 先頭 5ms: 0.0 → 1.0 のリニアゲイン適用
   - 末尾 5ms: 1.0 → 0.0 のリニアゲイン適用
   - フレーム単位で全チャンネルに適用

2. **ドレイン時間延長（300ms）**
   ```cpp
   Sleep(DRAIN_WAIT_MS);
   ```
   - バッファ内の残留データが完全に再生されるまで待機
   - SBC コーデックのレイテンシに対応

### 効果

- 波形不連続によるクリック音を大幅に軽減
- 通知音のアタック感を損なわない（5ms は業界標準）

## WAV 直接読み込み機構

### 目的

Media Foundation のリサンプリングによる音質劣化を回避し、WAV ファイルを可能な限りネイティブ品質で再生する。

### 条件

以下の条件を **すべて** 満たす場合に直接読み込みを実行：

1. ファイル形式が RIFF/WAVE
2. フォーマットタグが `WAVE_FORMAT_PCM` または `WAVE_FORMAT_IEEE_FLOAT`
3. サンプルレートがデバイスのミックスフォーマットと一致

チャンネル数が不一致の場合（例：モノラル→ステレオ）は、直接読み込み後に `ConvertFormat()` でチャンネル変換を行う。Media Foundation 経由の変換で発生する約 -3dB の音量低下を回避するための設計。

### 実装

```cpp
bool TryReadWavDirect(const wchar_t* filePath, std::vector<float>& audioData,
                      UINT32 targetSampleRate, UINT32 targetChannels)
```

#### 処理フロー

1. RIFF/WAVE ヘッダー検証
2. `fmt ` チャンク読み込み＆フォーマット検証
3. `WAVE_FORMAT_EXTENSIBLE` の場合、SubFormat から実際のフォーマットタグを取得
4. `data` チャンク検索＆サイズ取得
5. PCM データ読み込み＆ float32 変換
   - int16 PCM: `/32768.0f`
   - int24 PCM: `/8388608.0f`（符号拡張あり）
   - int32 PCM: `/2147483648.0f`
   - float32: 直接読み込み

#### フォールバック

条件を満たさない場合は `false` を返し、呼び出し元が Media Foundation デコードにフォールバックする。

```cpp
bool decoded = TryReadWavDirect(filePath, decodedData, ...);
if (!decoded) {
    decoded = DecodeAudioFile(filePath, decodedData, ...);
}
```

### 効果

- リサンプリングなしで音質劣化を完全に回避
- WAV ファイルの高速読み込み（MF デコーダをバイパス）

## WASAPI セッション仕様

### モード

- **共有モード（Shared Mode）**
  - システムミキサーを経由
  - 他のアプリケーションと同時再生可能
  - デバイスのミックスフォーマットに従う

### イベント駆動

```cpp
audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        0, 0, mixFormat, nullptr);
```

- バッファ空き領域が発生するとイベントがシグナル状態になる
- `WaitForSingleObject` でイベント待機
- バッファパディングを確認して空き領域分だけデータ書き込み

### バッファ管理

```cpp
UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
UINT32 framesToWrite = (std::min)(numFramesAvailable, totalFrames - frameIndex);
```

- デバイスバッファサイズを取得（`GetBufferSize`）
- 現在のパディング量を取得（`GetCurrentPadding`）
- 空き領域に書き込み可能なフレーム数を計算
- オーバーランを防ぐため残データ量も考慮

## パフォーマンス特性

### ファイルサイズ

- 実行ファイル: 約 212KB
- ランタイム依存なし（すべて Windows 組み込み）

### 起動時間

- 数十ミリ秒（通知音用途に最適化）
- バックグラウンド実行対応（コンソールウィンドウなし）

### メモリ使用量

- 最小限のメモリフットプリント
- オーディオデータは `std::vector<float>` で管理
- デコード後は即座に MF をシャットダウンしてメモリ解放

## Opus デコード機構

### 目的

Opus は低遅延・高音質を特徴とする音声コーデックで、通知音やVoIP アプリケーションで広く使用される。Windows Media Foundation では Opus 対応が限定的（Windows 11 22H2 以降の一部環境のみ）であるため、libopus を静的リンクして確実な対応を実現する。

### 対応コンテナ

- `.opus` ファイル（Ogg Opus コンテナ）
- `.ogg` ファイル（Opus コーデックのみ対応、Vorbis は非対応）

### 実装

```cpp
bool TryDecodeOpusFile(const wchar_t* filePath, std::vector<float>& audioData,
                       UINT32 targetSampleRate, UINT32 targetChannels)
```

#### 処理フロー

1. **Ogg コンテナパース**
   - libogg でページ単位にデータを読み込み
   - ストリームの初期化とシリアル番号の取得

2. **OpusHead パケット解析**
   - 最初のパケットから Opus ストリームの識別
   - チャンネル数などのメタデータを取得
   - Opus デコーダを 48kHz で初期化（Opus ネイティブレート）

3. **オーディオパケットデコード**
   - opus_decode_float() で PCM float32 に変換
   - 全パケットをデコードしてバッファに蓄積

4. **フォーマット変換**
   - ConvertFormat() でサンプルレート変換（48kHz → デバイスレート）
   - チャンネル数変換（必要に応じてモノラル↔ステレオ）

#### リサンプリング

簡易リニアLaTeX 補間によるリサンプリングを実装：
- 通知音は短時間（数秒以下）のため、高品質なリサンプラーは不要
- デバイスが 48kHz の場合はリサンプリング不要（Opus ネイティブ）

#### エラーハンドリング

- Opus ファイルでない場合は `false` を返し、Media Foundation にフォールバック
- デコードエラーも同様にフォールバックで対処

### 効果

- Windows 10 1607 以降で確実に Opus 再生が可能
- ファイルサイズ増加約 197KB（211KB → 408KB）
- 環境依存なし（静的リンク）

## 対応フォーマット

### libopus 経由

- Opus (.opus, .ogg)

### Windows Media Foundation 経由

- MP3
- AAC (M4A)
- FLAC
- WMA
- その他 WMF がサポートする形式

### WAV 直接読み込み

- PCM int16
- PCM int24
- PCM int32
- IEEE float32

※ WAVE_FORMAT_EXTENSIBLE にも対応

## 制約事項

### サンプルレート

WASAPI デバイスのミックスフォーマットに自動変換される。一般的には 48000Hz。

### チャンネル数

WASAPI デバイスのミックスフォーマットに自動変換される。一般的にはステレオ（2ch）。

### 再生デバイス

デフォルトオーディオデバイス固定。デバイス選択機能はなし。

### 同時再生

単一ファイルのみ。複数ファイルの同時再生は非対応。

## セキュリティ

### ファイルアクセス

- 指定されたファイルパスのみアクセス
- ディレクトリトラバーサル対策なし（ローカル用途想定）

### 入力検証

- ファイル存在確認のみ
- ファイル内容の悪意検証なし（信頼できるファイル想定）

## メンテナンス

### コーディングスタイル

- 既存コードに準拠
- エラーハンドリングは終了コードで明確に区別
- パフォーマンスと軽量性を重視

### ドキュメント

- `README.md`: ユーザー向け仕様書（常に最新状態を維持）
- `spec.md`: 開発者向け技術仕様書
- `.claude/CLAUDE.md`: プロジェクト開発規則
