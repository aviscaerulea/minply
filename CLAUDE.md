# CLAUDE.md - minply

@README.md
@spec.md

## ドキュメント管理規則

### README.md（外部仕様）

- 機能追加や変更を行った際は、必ず README.md に反映する
- 仕様、使用方法、動作要件、ビルド方法などは全て README.md に記載する

### spec.md（技術仕様）

- アーキテクチャ、実装詳細、技術的根拠は spec.md に記載する

### 共通

- 機能や仕様の変更は漏らさず README.md と spec.md の両方に随時反映する
- コード変更とドキュメントの更新は同一コミットで行う

## 開発規則

### ビルド環境

- PowerShell は pwsh.exe（PowerShell 7+）を使用
- Visual Studio ツール群は `Enable-VSDev` で有効化
- Task による自動ビルドスクリプトを優先使用

### ビルド＆テスト手順

- ビルド: `Enable-VSDev` → `task build`
- テスト: `.\test.bat`（PowerShell から実行）

### リリース手順

- アセット命名: `minply-{version}-x64.zip`（exe のみ同梱）
- リリースノート: 日本語、`## 変更点` → `### {カテゴリ}` の構造
- バージョニング: 機能追加は minor、バグ修正は patch

### コーディング規則

- C++ のコーディングスタイルは既存コードに準拠
- エラーハンドリングは終了コードで明確に区別（0-5 の範囲）
- パフォーマンスと軽量性を重視（実行ファイルサイズに注意）
- MSVC の `std::min`/`std::max` は型を厳密に一致させる（`DWORD` と `unsigned int` 混在不可）

### 設計制約

- BLE レシーバのスリープ回避のため WASAPI セッションは分割しない（単一セッション内でガードトーン→本編→ガードトーンを順次再生）
