# minply 仕様

minply は、通知音や読み上げ音声を Windows の既定のオーディオデバイスで鳴らす CLI オーディオプレイヤーだ。導入の手順と使用例は README.md にある。

## 機能

要件は EARS 形式で書き、機能のまとまりごとの接頭辞と 1 からの連番を振り、番号を変えず、削除した番号は欠番のまま残す。

| 起動の形 | 要件の接頭辞 |
| --- | --- |
| `minply.exe [--session <id>] [オーディオファイル \| -]` | `CLI`、`PLY`、`GRD`、`LUD`、`VOL`、`MUT`、`SES` |
| `minply.exe --tray` | `TRY` |
| `minply.exe --uninstall` | `UNI` |

`CFG`、`LOG`、`EXT` はすべての起動の形に共通する。

### コマンドライン

- CLI-1：minply は次の 3 つの形の起動を受け付ける
  - `minply.exe [--session <id>] [オーディオファイル | -]`
  - `minply.exe --tray`
  - `minply.exe --uninstall`
- CLI-2：引数が CLI-1 のどの形にも当たらない場合、minply は使い方を標準エラー出力に出し、引数を無効として扱う
  - 例：同じオプションの重複、`--session` の値の欠落、2 つ目のパス、`--tray` か `--uninstall` と他の引数の併用

### 再生

- PLY-1：オーディオファイルのパスを受けたとき、minply はそのファイルを再生し、再生が終わり次第終了する
- PLY-2：パスに `-` を受けたとき、minply は標準入力から読んだ音声データを再生する
- PLY-3：パスを省略し、標準入力がパイプかファイルのとき、minply は標準入力から読んだ音声データを再生する
- PLY-4：パスを省略して標準入力から音声データを読めない場合、minply は何も再生せずに終了コード 0 で終わる
- PLY-5：minply は Opus、MP3、WAV、AAC、FLAC、WMA の音声を再生する
- PLY-6：minply は Opus をモノラルとステレオ（channel mapping family 0）に限って再生する
- PLY-7：minply は Windows の既定のオーディオデバイスにだけ出力する
- PLY-8：minply はウィンドウを出さずに再生する

### ガードトーン

人の耳に聞こえない高周波のトーンで Bluetooth レシーバを起こしたまま保ち、本編の先頭が削れないようにする。

- GRD-1：`[guard] enabled` が true の構成では、minply は本編の前後に不可聴のガードトーンを流す
- GRD-2：minply はガードトーンを、本編の前に `lead_in_duration` 秒、本編の後に `lead_out_duration` 秒流す
- GRD-3：minply はガードトーンを、`frequency` の周波数（Hz）と `amplitude` の振幅で作る

### ラウドネスノーマライズ

- LUD-1：`[loudness] enabled` が true の構成では、minply は本編のラウドネスを EBU R128 で測り、`target`（LUFS）にそろえる
- LUD-2：`[loudness] enabled` が true の構成では、minply は本編のサンプルピークを `peak_ceiling` 以下に抑える

### 音量の規則

指定したアプリの状態に応じて、本編の音量を下げる。

- VOL-1：ミュート条件に当たらずに再生するとき、minply は `[[volume]]` を設定ファイルに書いた順に照合する
- VOL-2：`[[volume]]` が当たったとき、minply は最初に当たった規則の `volume`（%）を本編の音量に掛ける
- VOL-3：どの `[[volume]]` も当たらないとき、minply は本編の音量を変えない
- VOL-4：minply は `volume` の割合をラウドネスノーマライズの後の本編に掛け、ガードトーンには掛けない
- VOL-5：minply は `[[volume]]` の `process`、`title`、`playing`、`hold` を `[[mute]]` と同じ規則（MUT-5〜MUT-13）で照合する
- VOL-6：`process` か `volume` がないか、不正な値を含む `[[volume]]` がある場合、minply は警告を出してその規則を丸ごと無視する
  - 不正な値：0〜100 の整数でない `volume`、MUT-14 が不正とする `process`、`title`、`playing`、`hold`
- VOL-7：minply は `minply.local.toml` の `[[volume]]` を `minply.toml` の分に足して読み込む
- VOL-8：`[[volume]]` が当たったとき、minply は割合と当たった条件をログに書く
- VOL-9：`--session` で再生するとき、minply は常駐プロセスの起動時に読んだ `[[volume]]` で照合する

### ミュート条件

再生を求められたときに鳴らさない条件を判定する。手動ミュートとマイク/カメラの切り替えの操作は「タスクトレイ」に書く。

- MUT-1：次のどれかに当たる間に再生を求められたとき、minply は再生せずに終了コード 0 で終わる
  - トレイで手動ミュートしている
  - トレイの「マイク/カメラ使用中はミュートにする」がオンで、マイクかカメラを使っているアプリがある
  - `[[mute]]` の規則のどれかが当たる
- MUT-2：ミュート条件に当たって再生しなかったとき、minply は再生しなかった理由をログに書く
- MUT-3：ミュート条件に当たったとき、minply は入力ファイルがなくても終了コード 0 で終わる
- MUT-4：標準入力からの再生でミュート条件に当たったとき、minply は標準入力を最後まで読み捨ててから終わる
- MUT-5：minply は 1 つの `[[mute]]` に書いた条件をすべて満たしたときにだけ、その規則を当てる
- MUT-6：minply は、`process` の exe 名と大文字小文字を区別せずに一致するアプリの起動中に、`process` の条件を満たしたとみなす
- MUT-7：`title` を書いた構成では、minply は `process` のウィンドウタイトルの一部が正規表現に当たるときに条件を満たしたとみなす
  - 正規表現は大文字小文字を区別しない
- MUT-8：minply はウィンドウタイトルの照合に各ウィンドウの表示中のタイトルだけを使い、ブラウザの裏のタブを対象にしない
- MUT-9：`playing = true` の構成では、minply は `process` のアプリがどれかの出力デバイスで音を出すときに条件を満たしたとみなす
- MUT-10：minply は音を出しているかをアプリ単位で判定し、ブラウザで音を出しているタブを区別しない
- MUT-11：`playing = true` の構成では、トレイの起動中、minply はアプリが最後に音を出してから `hold` 秒の間も条件を満たしたとみなす
- MUT-12：トレイが起動していない間、minply は `playing` の条件をその瞬間の音量だけで判定する
- MUT-13：`hold` が 0 の構成では、minply は `playing` の条件をその瞬間の音量だけで判定する
- MUT-14：`process` がないか不正な値を含む `[[mute]]` がある場合、minply は警告を出してその `[[mute]]` を丸ごと無視する
  - 不正な値：空か文字列でない `process`、文字列でないか不正な正規表現の `title`、true と false 以外の `playing`、範囲外の `hold`
- MUT-15：minply は `minply.local.toml` の `[[mute]]` を `minply.toml` の分に足して読み込む
- MUT-16：`[mute] processes` か `[[mute.window]]` を読んだ場合、minply は警告を出してその設定を使わない

マイクとカメラの使用中の判定は、Windows のプライバシー機能が残す使用記録に頼る。

### セッション常駐

- SES-1：`--session <id>` を付けて再生を求められたとき、minply は id ごとの常駐プロセスで再生する
- SES-2：常駐プロセスで再生したとき、minply は最後の再生の終了から `[linger] duration` 秒の間、ガードトーンを流して常駐する
- SES-3：最後の再生の終了から `[linger] duration` 秒が過ぎたとき、minply は常駐プロセスを終了する
- SES-4：同じ id の常駐中に再生を求められたとき、minply はリードインを流さずに直ちに再生する
- SES-5：minply は同じ id への再生要求を 1 つずつ順に処理する
- SES-6：minply は異なる id への再生要求を互いに独立して同時に鳴らす
- SES-7：id が半角英数字と `.`、`_`、`-` からなる 1〜64 文字でない場合、minply は引数を無効として扱う
- SES-8：常駐プロセスの動作中、minply はガードトーン、ラウドネス、常駐秒数の設定を読み直さない
- SES-9：オーディオデバイスが切り替わった後に再生を求められたとき、minply は常駐プロセスを起動し直してから再生する

### タスクトレイ

- TRY-1：`--tray` で起動したとき、minply はタスクトレイに常駐する
- TRY-2：別のトレイが動いている間に `--tray` で起動したとき、minply は古いトレイを終了させて入れ替わる
- TRY-3：メニューで手動ミュートを選んだとき、minply は期限なしか、選んだ時間だけ手動ミュートにする
- TRY-4：手動ミュートの時間が過ぎたとき、minply は手動ミュートを解く
- TRY-5：minply はメニューに出す手動ミュートの時間を `[tray] mute_durations` から作る
- TRY-6：アイコンを左クリックしたとき、minply は手動ミュート中なら解除し、そうでなければ期限なしで手動ミュートにする
- TRY-7：メニューで「マイク/カメラ使用中はミュートにする」を切り替えたとき、minply はその設定をトレイの再起動後も保つ
- TRY-8：メニューでスタートアップ登録をオンにしたとき、minply はログオン時にトレイを自動で起動する
- TRY-9：メニューで設定ファイルかログファイルを開く操作をしたとき、minply はそのファイルを既定のアプリで開く
- TRY-10：鳴らさない状態の間、minply はアイコンに理由の種類を示す赤い印を付ける
  - ミュート条件（マイクやカメラの使用、`[[mute]]`）：右下がりの斜線（＼）
  - 手動ミュート：右上がりの斜線（／）
  - 両方：×印
- TRY-11：アイコンにマウスを載せたとき、minply は鳴らさない理由を表示する
- TRY-12：再生要求を受けたとき、minply はミュートで鳴らさなかった場合も含めてアイコンの右下に緑の点を 3 秒間付ける
- TRY-13：minply はミュート条件を `[tray] condition_interval` 秒ごとに確かめて、アイコンに反映する
- TRY-14：設定ファイルが変わったとき、minply はトレイを再起動せずに、アイコンに出すミュート条件の判定へ反映する
- TRY-15：トレイを終了したとき、minply は手動ミュートを解く
- TRY-16：minply は `[tray]` の設定をトレイの起動時にだけ読む
- TRY-17：`mute_durations` に不正な時間の要素がある場合、minply は警告を出してその要素だけを除く

「マイク/カメラ使用中はミュートにする」の既定はオフだ。

### ログ

- LOG-1：エラーか設定の警告を出すとき、minply は標準エラー出力に加えて `logs\YYYY-MM-DD.log` に 1 行ずつ書く
  - `logs` は `minply.exe` と同じフォルダーにある
- LOG-2：minply は再生の成功をログに書かない
- LOG-3：トレイを起動したとき、minply は今日を含めて `[log] retention_days` 日より古いログを削除する
- LOG-4：`retention_days` が 0 の構成では、minply はログを削除しない

### 設定ファイル

- CFG-1：minply は exe と同じフォルダーの `minply.toml` を読んだ後に `minply.local.toml` を読み、後者の値を優先する
- CFG-2：設定ファイルか設定項目がない場合、minply はその項目を既定値で動かす
- CFG-3：設定値が許容範囲外か不正な場合、minply は警告を標準エラー出力とログへ出し、その項目を既定値に戻す
  - `[[mute]]` と `[[volume]]` は除く（MUT-14、VOL-6）

| セクション | 項目 | 既定値 | 許容範囲 |
| --- | --- | --- | --- |
| `guard` | `enabled` | `true` | `true`、`false` |
| `guard` | `lead_in_duration` | `1.2` | 0.0〜10.0 |
| `guard` | `lead_out_duration` | `1.2` | 0.0〜10.0 |
| `guard` | `frequency` | `19000.0` | 20.0〜20000.0 |
| `guard` | `amplitude` | `0.001` | 0.0〜1.0 |
| `loudness` | `enabled` | `true` | `true`、`false` |
| `loudness` | `target` | `-16.0` | -70.0〜0.0 |
| `loudness` | `peak_ceiling` | `0.891` | 0.001〜1.0 |
| `linger` | `duration` | `30.0` | 1.0〜600.0 |
| `log` | `retention_days` | `7` | 0〜3650 の整数 |
| `[[mute]]`、`[[volume]]` | `process` | なし（必須） | 空でない exe 名 |
| `[[mute]]`、`[[volume]]` | `title` | なし | 正規表現 |
| `[[mute]]`、`[[volume]]` | `playing` | `false` | `true`、`false` |
| `[[mute]]`、`[[volume]]` | `hold` | `30.0` | 0.0〜3600.0 |
| `[[volume]]` | `volume` | なし（必須） | 0〜100 の整数 |
| `tray` | `condition_interval` | `5.0` | 1.0〜60.0 |
| `tray` | `mute_durations` | `["30m", "60m", "90m", "12h"]` | 整数に `s`、`m`、`h` を付けた 1 秒〜7 日 |

### 終了コード

- EXT-1：処理を正常に終えたとき、minply は終了コード 0 で終わる
- EXT-2：引数が無効な場合、minply は終了コード 1 で終わる
- EXT-3：入力の音声データを用意できない場合、minply は終了コード 2 で終わる
  - ファイルが見つからない、読めない、空、`-` を受けて標準入力から読めない
- EXT-4：デコードに失敗した場合、minply は終了コード 3 で終わる
  - 4GB を超える入力ファイルを含む
- EXT-5：オーディオデバイスの初期化に失敗した場合、minply は終了コード 4 で終わる
- EXT-6：再生に失敗した場合、minply は終了コード 5 で終わる
  - 常駐プロセスの起動か通信の失敗、トレイの起動失敗、`--uninstall` の削除の失敗を含む

### アンインストール

- UNI-1：`--uninstall` で起動したとき、minply は動いているトレイを閉じる
- UNI-2：`--uninstall` で起動したとき、minply はレジストリの設定とスタートアップ登録と `%LOCALAPPDATA%\minply` を削除する
- UNI-3：`--uninstall` で起動したとき、minply は exe と同じフォルダーの設定ファイルと `logs` を削除しない

## インストール

README.md の「インストール」節を参照する。

## 使い方

README.md の「使い方」節と「設定」節を参照する。

## ライセンス

LICENSE（MIT License）を参照する。
