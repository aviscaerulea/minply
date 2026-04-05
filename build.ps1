# minply ビルドスクリプト
# Visual Studio の開発環境を pwsh で有効化してコンパイルを実行

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# vcpkg パス設定（Scoop シム経由インストールに対応）
$vcpkgCmd = (Get-Command vcpkg -ErrorAction Stop).Source
$shimFile = [System.IO.Path]::ChangeExtension($vcpkgCmd, ".shim")
if (Test-Path $shimFile) {
    # Scoop のシムファイルから実体パスを取得
    $vcpkgReal = (Get-Content $shimFile |
        Where-Object { $_ -match "^path" } |
        ForEach-Object { ($_ -split '"')[1] } |
        Select-Object -First 1)
    $vcpkgRoot = Split-Path $vcpkgReal
}
else {
    $vcpkgRoot = Split-Path $vcpkgCmd
}
$vcpkgInclude = "$vcpkgRoot\installed\x64-windows-static\include"
$vcpkgLib = "$vcpkgRoot\installed\x64-windows-static\lib"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "vswhere.exe が見つからない: $vswhere"; exit 1 }
$vsPath = & $vswhere -products '*' -latest -property installationPath
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

# 出力ディレクトリ作成
if (-not (Test-Path "out")) {
    New-Item -ItemType Directory -Path "out" | Out-Null
}

Write-Host "Compiling resources..." -ForegroundColor Cyan

rc /nologo /fo out\minply.res src\minply.rc 2>&1 | Tee-Object -FilePath "out/build.log"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Resource compile failed" -ForegroundColor Red
    exit 1
}

Write-Host "Compiling src\minply.cpp..." -ForegroundColor Cyan

# コンパイル実行（ログを out/build.log に追記）
cl /nologo /EHsc /O2 /MT /std:c++17 /W3 /utf-8 `
   /I"$vcpkgInclude" `
   /Fo:out/ /Fe:out/minply.exe src\minply.cpp out\minply.res `
   ole32.lib mfplat.lib mfreadwrite.lib mfuuid.lib `
   "$vcpkgLib\opus.lib" "$vcpkgLib\ogg.lib" `
   /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup 2>&1 | Tee-Object -Append -FilePath "out/build.log"

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Build successful" -ForegroundColor Green
Get-Item out/minply.exe
