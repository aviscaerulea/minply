# minply

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/minply)](https://github.com/aviscaerulea/minply/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/minply)](LICENSE)
[![Build](https://github.com/aviscaerulea/minply/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/minply/actions/workflows/release.yml)

通知音や読み上げ音声を鳴らすための、Windows 向け超軽量 CLI オーディオプレイヤーです。

Bluetooth レシーバは無音が続くと省電力状態に入り、次に鳴らした音の先頭を削ってしまいます。minply は人の耳に聞こえない高周波の音を本編の前後に流し続けることで、この先頭欠損を防ぎます。実行ファイルは約 690KB ひとつだけで、追加のランタイムは要りません。

## 機能

- 先頭欠損の防止：不可聴音でレシーバを起こしたまま保ち、音の出だしが切れないようにする
- セッション常駐：連続再生のたびに発生する待ち時間をなくす
- 幅広い形式：Opus, MP3, WAV, AAC, FLAC, WMA などを再生
- 標準入力からの再生：ファイルパスの代わりにパイプで音声データを渡せる
- 単一実行ファイル：インストール不要で、置くだけで動く
- 即時終了：ウィンドウを出さずに再生し、終わり次第すぐ終了する
- ミュート条件：指定アプリの起動、ブラウザでの特定サイトの表示、マイクやカメラの使用を検知して消音する
- 手動ミュート：タスクトレイのメニューから、時間を決めて、または期限なしで鳴らさないようにする

## インストール

### 動作要件

- Windows 10 version 1607 以降（x64）
- 追加のソフトウェアは不要（依存関係はすべて Windows に組み込み済み）

### 手順

#### リリースの ZIP から

[リリースページ](https://github.com/aviscaerulea/minply/releases/latest)から `minply-<バージョン>-x64.zip` をダウンロードします。展開して出てくる `minply.exe` を任意のフォルダへ置けば使えます。同梱の `minply.toml` を同じフォルダへ置くと動作を調整できます。

#### Scoop から

[Scoop](https://scoop.sh/) を使う場合は、バケットを追加してインストールします。

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install minply
```

#### アンインストール

タスクトレイの設定とスタートアップ登録は Windows のレジストリに残ります。`--uninstall` は動いているトレイを閉じ、レジストリの設定と旧版のログ（`%LOCALAPPDATA%\minply`）を削除します。minply を消す前に、次のコマンドを実行します。

```powershell
minply.exe --uninstall
```

設定ファイルとログ（`logs` フォルダ）は `minply.exe` と同じフォルダにあり、このコマンドでは消えません。ZIP から入れた場合は、フォルダごと削除します。Scoop の場合は `scoop uninstall minply --purge` を実行すると、設定ファイルとログも消えます。

## 使い方

```
minply.exe [--session <id>] [オーディオファイル | -]
minply.exe --tray
minply.exe --uninstall
```

```powershell
# ファイルを再生
minply.exe notification.mp3

# エラー出力をキャプチャ
minply.exe notfound.mp3 2>&1 | Out-File error.log
```

### 標準入力からの再生

引数を省略するか `-` を指定すると、標準入力から音声データを読み込んで再生します。

```cmd
:: cmd.exe からのパイプ（引数省略可）
type alert.mp3 | minply.exe
```

```powershell
# PowerShell 7 以降からのパイプ
Get-Content -AsByteStream -Raw alert.mp3 | minply.exe
```

> [!IMPORTANT]
> PowerShell の `Get-Content` は既定でテキストとして読むためバイナリが壊れます。パイプで渡すときは `-AsByteStream` を必ず指定してください（PowerShell 7 以降）。

### 連続再生の高速化

`--session <id>` を付けると、再生後もプロセスがしばらく常駐して不可聴音を流し続けます。レシーバが起きたままになるため、同じ id での次の再生は待ち時間なしで始まります。読み上げ音声を続けて鳴らす用途で効果があります。

```powershell
# セッション ID を渡す例
minply.exe --session $env:CLAUDE_SESSION_ID speech.wav
```

- 初回：常駐プロセスを起動し、レシーバを起こしてから再生
- 2 回目以降：同じ id なら待ち時間なしで再生
- 最後の再生から一定時間が過ぎると常駐プロセスは自動で終了

同じ id への再生要求は 1 つずつ順番に処理し、異なる id は互いに独立して同時に鳴ります。続けて鳴らす音を重ねたくない場合は同じ id を使ってください。

id に使えるのは半角英数字と `.`、`_`、`-` で、長さは 1〜64 文字です。

### タスクトレイ

`minply.exe --tray` で起動すると、タスクトレイに常駐します。すでにトレイが動いているときは、新しく起動したトレイが古いトレイを終了して入れ替わります。アイコンの右クリックメニューで、次の操作ができます。

| 操作 | 内容 |
| --- | --- |
| 手動ミュート | 期限なし、または設定した時間（既定は 30 分間、60 分間、90 分間、12 時間）だけ鳴らさない |
| マイク/カメラ使用中はミュートにする | 会議などでマイクかカメラを使っている間は鳴らさない（既定はオフ） |
| スタートアップ登録 | ログオン時にトレイを自動で起動する |
| 設定ファイルを開く、ログファイルを開く | それぞれのファイルを既定のアプリで開く |

手動ミュート中と、マイクやカメラの使用、設定ファイルのミュート条件で鳴らさない間は、アイコンに赤い斜線が付きます。アイコンにマウスを載せると、鳴らさない理由が出ます。

アイコンを左クリックすると、手動ミュート中なら解除し、そうでなければ期限なしでミュートします。

再生要求を受けると、ミュートで鳴らさなかった場合も含めて、アイコンの右下に緑の点が 3 秒間付きます。

トレイを終了すると手動ミュートは解けます。マイクとカメラの切り替えとスタートアップ登録は、トレイを再起動しても変わりません。

### 終了コード

| コード | 説明 |
| --- | --- |
| 0 | 成功 |
| 1 | 引数が無効 |
| 2 | ファイルが見つからない、読めない、空 |
| 3 | デコード失敗 |
| 4 | オーディオデバイスの初期化失敗 |
| 5 | 再生失敗（常駐プロセスの起動と通信の失敗、トレイの起動失敗、`--uninstall` の削除失敗を含む） |

### ログ

エラーと設定ファイルの警告は、標準エラー出力に加えて `minply.exe` と同じフォルダの `logs\YYYY-MM-DD.log` にも 1 行ずつ記録し、再生の成功は記録しません。フックから呼んだときや `--session` の常駐中は標準エラー出力が見えないため、鳴らなかった原因はこのログで調べてください。トレイを起動するたびに、既定では直近 7 日分を残して古いログを削除します。

## 設定

実行ファイルと同じフォルダに `minply.toml` を置くと動作を調整できます。無い場合は既定値で動きます。`minply.local.toml` を併せて置くと、その内容が後から設定を上書きします。個人環境だけの設定に使ってください。

各項目の意味と既定値は `minply.toml` のコメントに書いてあります。主な項目は次のとおりです。

| セクション | 項目 | 内容 |
| --- | --- | --- |
| `guard` | `enabled` | 不可聴音による先頭欠損の防止を使うか |
| `guard` | `lead_in_duration` | 本編の前に流す秒数 |
| `guard` | `lead_out_duration` | 本編の後に流す秒数 |
| `loudness` | `enabled` | 音源ごとの音量差をそろえるか |
| `loudness` | `target` | そろえる先の音量 |
| `linger` | `duration` | `--session` 指定時に常駐し続ける秒数 |
| `log` | `retention_days` | トレイの起動時にログを残す日数（0 で削除しない） |
| `mute` | `processes` | 起動していると再生しないアプリ（exe 名） |
| `mute.window` | `process`、`title` | 指定アプリのウィンドウタイトルが正規表現に当たると再生しない |
| `tray` | `mute_durations` | トレイメニューに出す手動ミュートの時間 |

許容範囲外の値や不正な値は、警告を標準エラー出力とログへ出したうえで既定値に戻します。

## 制限事項

- 出力先は Windows の既定のオーディオデバイスに固定で、再生先は選べない
- Opus はモノラルとステレオのみ対応する（channel mapping family 0）
- `--session` の常駐中は設定ファイルを読み直さない（変更は常駐の終了後の再生から有効）
- オーディオデバイスを切り替えても常駐プロセスはすぐに終わらず、次の呼び出しで終了して起動し直す
- ウィンドウタイトルの条件は各ウィンドウの表示中のタブにだけ当たり、裏のタブのサイトは対象外
- トレイの設定（`mute_durations`）はトレイの起動時にだけ読む
- ミュート条件によるアイコンの斜線は、条件が成り立ってから最大 5 秒遅れて付き、条件が外れてから消えるまでも同じだけ遅れる
- マイクとカメラの使用中の判定は、Windows のプライバシー機能が残す使用記録に頼る
- ミュート条件に当たると、ファイルがなくても再生せずに終了コード 0 で終わり、理由をログに残す

## ビルド

`task build` は実行時に vcpkg の依存ライブラリを自動でインストールします。

```powershell
# 通常ビルド（依存ライブラリの自動インストールを含む）
task build

# PowerShell から直接ビルドする場合（事前に vcpkg install が必要）
pwsh -ExecutionPolicy Bypass -File build.ps1
```

言語は C++17（MSVC）です。デコードに Windows Media Foundation、音声出力に WASAPI を使います。以下のライブラリを静的リンクしています。

- [libopus](https://opus-codec.org/)：Opus コーデック
- [libogg](https://xiph.org/ogg/)：Ogg コンテナ
- [libebur128](https://github.com/jiixyj/libebur128)：EBU R128 ラウドネス測定
