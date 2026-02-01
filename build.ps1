# minply ビルドスクリプト
# Visual Studio の開発環境を pwsh で有効化してコンパイルを実行

$ErrorActionPreference = "Stop"

# Visual Studio 開発環境を有効化
Enable-VSDev -Arch amd64

# 出力ディレクトリ作成
if (-not (Test-Path "out")) {
    New-Item -ItemType Directory -Path "out" | Out-Null
}

Write-Host ""
Write-Host "Compiling minply.cpp..." -ForegroundColor Cyan

# コンパイル実行（ログを out/build.log に保存）
cl /nologo /EHsc /O2 /MT /std:c++17 /W3 /Fo:out/ /Fe:out/minply.exe minply.cpp ole32.lib mfplat.lib mfreadwrite.lib mfuuid.lib /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup 2>&1 | Tee-Object -FilePath "out/build.log"

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Build successful" -ForegroundColor Green
Get-Item out/minply.exe
