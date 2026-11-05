# minply ビルドスクリプト
# Visual Studio の開発環境を pwsh で有効化してコンパイルを実行
#
# 引数:
#   -Version  埋め込むバージョン文字列（例：v1.9.0、1.9.0-3-gabc1234-dirty）。先頭の v は除く

param(
    [string]$Version = "0.0.0"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# vcpkg パス設定（VCPKG_INSTALLATION_ROOT 環境変数 → Scoop シム の優先順）
if ($env:VCPKG_INSTALLATION_ROOT) {
    $vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
}
else {
    $vcpkgCmd = (Get-Command vcpkg -ErrorAction Stop).Source
    $shimFile = [System.IO.Path]::ChangeExtension($vcpkgCmd, ".shim")
    if (Test-Path $shimFile) {
        $vcpkgReal = (Get-Content $shimFile |
            Where-Object { $_ -match "^path" } |
            ForEach-Object { ($_ -split '"')[1] } |
            Select-Object -First 1)
        $vcpkgRoot = Split-Path $vcpkgReal
    }
    else {
        $vcpkgRoot = Split-Path $vcpkgCmd
    }
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

New-Item -ItemType Directory -Path "out" -Force | Out-Null

# バージョン定義ヘッダの生成
# rc.exe の /d では文字列リテラルを渡せないため、ヘッダを生成して .rc から include する。
# 数値版は先頭の major.minor.patch のみを採り、取れなければ 0,0,0,0 とする
$Version = $Version -replace '^v', ''
$verMatch = [regex]::Match($Version, '^(\d+)\.(\d+)\.(\d+)')
$verNum = if ($verMatch.Success) {
    "$($verMatch.Groups[1].Value),$($verMatch.Groups[2].Value),$($verMatch.Groups[3].Value),0"
}
else {
    "0,0,0,0"
}
@(
    "#define APP_VERSION_NUM $verNum",
    "#define APP_VERSION_STR `"$Version`""
) | Set-Content -Path "out\version.rc.h" -Encoding ascii

Write-Host "Compiling resources..." -ForegroundColor Cyan

# /c65001：.rc の日本語コメントを CP932 で走査させないため
rc /nologo /c65001 /I out\ /fo out\minply.res src\minply.rc 2>&1 | Tee-Object -FilePath "out/build.log"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Resource compile failed" -ForegroundColor Red
    exit 1
}

Write-Host "Compiling src\minply.cpp..." -ForegroundColor Cyan

cl /nologo /EHsc /O2 /MT /std:c++17 /W3 /utf-8 `
   /I"$vcpkgInclude" `
   /Fo:out/ /Fe:out/minply.exe src\minply.cpp out\minply.res `
   ole32.lib mfplat.lib mfreadwrite.lib mfuuid.lib `
   "$vcpkgLib\opus.lib" "$vcpkgLib\ogg.lib" "$vcpkgLib\ebur128.lib" `
   /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup 2>&1 | Tee-Object -Append -FilePath "out/build.log"

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed" -ForegroundColor Red
    exit 1
}

# バージョン情報リソースの埋め込み検証
# 埋め込みが欠けてもビルドは通るため、ここで検出する
$verInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path "out/minply.exe").Path)
if ($verInfo.ProductVersion -ne $Version -or -not $verInfo.FileDescription) {
    Write-Host "Version resource check failed: ProductVersion='$($verInfo.ProductVersion)' expected='$Version'" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Build successful" -ForegroundColor Green
Get-Item out/minply.exe
