# minply 技術仕様書

## 概要

minply は Bluetooth Low Energy (BLE) レシーバの音声遅延と先頭欠損を補償する超軽量 CLI オーディオプレイヤー。Windows Media Foundation (WMF) と WASAPI を使用し、単一実行ファイル（約 212KB）として動作する。

## 技術スタック

### API とライブラリ

- **Windows Media Foundation (WMF)**: オーディオデコード
  - `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib`
  - MP3, AAC, FLAC, WMA など多様な形式に対応
- **WASAPI (Windows Audio Session API)**: オーディオ出力
  - イベント駆動共有モード
  - デフォルトオーディオデバイスへの再生
- **COM (Component Object Model)**: Windows API の基盤

### ビルド環境

- 言語: C++
- コンパイラ: Visual Studio 2019 以降（C++ デスクトップ開発ワークロード）
- ビルドツール: PowerShell 7+, Task
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
   └─ 失敗時は MF デコード（DecodeAudioFile）
7. フェードイン/アウト適用（ApplyFade）
8. 無音バッファ生成（GenerateSilence）
9. WASAPI セッション開始＆再生（PlayAudio）
   ├─ 無音 0.7 秒先行再生
   └─ オーディオデータ再生
10. バッファドレイン待機
11. リソース解放＆終了
```

### 定数

```cpp
constexpr float SILENCE_DURATION = 0.7f;   // 無音時間（秒）
constexpr float FADE_DURATION = 0.005f;    // フェード時間（秒）
constexpr DWORD BUFFER_WAIT_MS = 100;      // バッファ待機時間（ミリ秒）
constexpr DWORD DRAIN_WAIT_MS = 300;       // ドレイン待機時間（ミリ秒）
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

## BLE 遅延補償機構

### 目的

BLE オーディオレシーバは電力節約のため、音声信号が途切れるとスリープモードに入る。再度音声を受信すると起床するが、起床に約 0.5〜0.7 秒かかり、その間の音声データは欠損する。

### 実装

1. **単一 WASAPI セッションでの先行無音再生**
   - 無音 0.7 秒を先行再生してレシーバを起床
   - 続けて本編オーディオデータを再生
   - セッションを分割しない（分割すると再スリープする）

2. **実装詳細**
   ```cpp
   const std::vector<float>* sources[] = { &leadIn, &audioData };
   for (const auto* source : sources) {
       // 各データソースを順次再生
   }
   ```

3. **効果**
   - 無音期間でレシーバが起床し、セッション起動ノイズを吸収
   - 本編音声は先頭から欠損なく再生される

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

以下の条件を **すべて** 満たす場合のみ直接読み込みを実行：

1. ファイル形式が RIFF/WAVE
2. フォーマットタグが `WAVE_FORMAT_PCM` または `WAVE_FORMAT_IEEE_FLOAT`
3. サンプルレートがデバイスのミックスフォーマットと一致
4. チャンネル数がデバイスのミックスフォーマットと一致

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

## 対応フォーマット

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
- `SPEC.md`: 開発者向け技術仕様書
- `.claude/CLAUDE.md`: プロジェクト開発規則
