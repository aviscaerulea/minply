# CLAUDE.md - minply

## ドキュメント管理規則

- 機能の追加と変更、仕様の変更は、漏らさず README.md に反映する
  - 反映の前に README.md を読み、既存の記述と食い違わない形で書く
- 仕様、使用方法、動作要件、ビルド方法は README.md に記載する
- コード変更とドキュメントの更新は同一コミットにまとめる

## 開発環境

- PowerShell のスクリプトとコマンドは pwsh.exe（PowerShell 7 以降）で実行する
- build.ps1 が `Enter-VsDevShell` で Visual Studio の開発環境を読み込むため、事前の有効化は要らない

## ビルド方法

- `task build` でビルドする（手順の詳細は README.md の「ビルド」節）

## テスト方法

- PowerShell から `.\test.bat` を実行する

## リリース

- アセット名：`minply-{version}-x64.zip`（exe、minply.toml、`package/minply.local.toml` を同梱）
- バージョニング：機能追加は minor、バグ修正は patch を上げる

## 実装上の注意点

- C++ のコーディングスタイルは既存コードに合わせる
- エラーは README.md の「終了コード」節の 0〜5 で種類ごとに区別して返す
- 実行速度と軽量性を重視し、実行ファイルのサイズを抑える
- MSVC の `std::min`/`std::max` には同じ型の引数を渡す（`DWORD` と `unsigned int` も混ぜない）
- Bluetooth レシーバのスリープを避けるため、ガードトーン、本編、ガードトーンを 1 つの WASAPI セッションで順に再生する（セッションを分けない）

## 参考

- README.md
