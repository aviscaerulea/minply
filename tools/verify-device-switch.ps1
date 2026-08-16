# 既定デバイス切替の検知経路だけを狙って検証するスクリプト
#
# 常駐サーバは 3 通りの理由で終了しうる。linger 満了、デバイス消失、
# そして既定デバイス変更の検知（エンドポイント ID の毎要求比較）だ。
# 通常の linger（30 秒）では切替操作中に満了して自然終了してしまい、
# どの経路で終了したのか判別できない。
#
# linger を 5 分へ延ばして満了を排除し、かつ旧デバイスの接続を保つことで、
# ID 比較だけが検知手段になる状況を作る。
#
# 実行: pwsh -ExecutionPolicy Bypass -File tools\verify-device-switch.ps1

$ErrorActionPreference = 'Stop'

$root   = Split-Path $PSScriptRoot -Parent
$exe    = Join-Path $root 'out\minply.exe'
if (-not (Test-Path $exe)) { throw "minply.exe が見つからない（先に task build を実行する）: $exe" }

# 設定は実行ファイルと同じディレクトリから読まれる
$exeDir   = Split-Path $exe -Parent
$localCfg = Join-Path $exeDir 'minply.local.toml'

$work = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $work | Out-Null
$wav = Join-Path $work 'switch.wav'

function Invoke-Minply {
    param([string[]]$ArgList)
    $t = Get-Date
    $p = Start-Process -FilePath $exe -ArgumentList $ArgList -PassThru -NoNewWindow
    $p.WaitForExit()
    return [pscustomobject]@{
        Exit = $p.ExitCode
        Sec  = [math]::Round(((Get-Date) - $t).TotalSeconds, 2)
    }
}

function Get-ServerPids {
    return @(Get-Process minply -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
}

# 1kHz を 1.5 秒鳴らすだけの WAV を作る
function New-Beep {
    param([string]$Path, [int]$Rate = 48000)
    $count = [int]($Rate * 1.5)
    $fade  = [int]($Rate * 0.005)
    $s = New-Object int16[] $count
    $step = 2 * [math]::PI * 1000 / $Rate
    for ($i = 0; $i -lt $count; $i++) {
        $v = 0.5 * [math]::Sin($step * $i)
        if ($i -lt $fade) { $v *= $i / $fade }
        $tail = $count - 1 - $i
        if ($tail -lt $fade) { $v *= $tail / $fade }
        $s[$i] = [int16][math]::Round($v * 32767)
    }
    $dataSize = $count * 2
    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter($fs)
    try {
        $bw.Write([char[]]'RIFF'); $bw.Write([uint32](36 + $dataSize)); $bw.Write([char[]]'WAVE')
        $bw.Write([char[]]'fmt '); $bw.Write([uint32]16)
        $bw.Write([uint16]1); $bw.Write([uint16]1)
        $bw.Write([uint32]$Rate); $bw.Write([uint32]($Rate * 2))
        $bw.Write([uint16]2); $bw.Write([uint16]16)
        $bw.Write([char[]]'data'); $bw.Write([uint32]$dataSize)
        foreach ($x in $s) { $bw.Write($x) }
    }
    finally { $bw.Dispose(); $fs.Dispose() }
}

# 既存のローカル設定は退避し、終了時に必ず戻す
$hadCfg = Test-Path $localCfg
if ($hadCfg) { $savedCfg = Get-Content $localCfg -Raw }

try {
    New-Beep -Path $wav
    Set-Content -Path $localCfg -Value "[linger]`nduration = 300.0`n" -NoNewline -Encoding utf8
    Write-Host 'linger を 300 秒に設定した（満了による終了を排除するため）' -ForegroundColor Cyan
    Write-Host ''

    Get-Process minply -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 800

    Write-Host '=== 手順 1: 現在のデバイスで再生する ===' -ForegroundColor Yellow
    Read-Host '準備ができたら Enter'
    $r1 = Invoke-Minply @('--session', 'devsw', $wav)
    $before = Get-ServerPids
    Write-Host ("  exit={0} {1}s / 常駐サーバ PID: {2}" -f $r1.Exit, $r1.Sec, ($before -join ','))
    Write-Host ''

    Write-Host '=== 手順 2: 出力先を切り替える ===' -ForegroundColor Yellow
    Write-Host '  重要：旧デバイスの接続は切らず、Windows のサウンド設定で' -ForegroundColor Cyan
    Write-Host '        「既定の出力先」だけを別デバイスへ変更すること。' -ForegroundColor Cyan
    Write-Host '        電源オフや切断をするとデバイス消失の経路になり、検証にならない。' -ForegroundColor Cyan
    Read-Host '切り替えたら Enter'

    $mid = Get-ServerPids
    $stillAlive = ($mid | Where-Object { $before -contains $_ }).Count -gt 0
    Write-Host ("  切替直後の常駐サーバ: {0}" -f $(if ($stillAlive) { "生存 (PID $($mid -join ','))" } else { '既に終了' }))
    Write-Host ''

    Write-Host '=== 手順 3: 同じセッション ID で再生する ===' -ForegroundColor Yellow
    $r2 = Invoke-Minply @('--session', 'devsw', $wav)
    $after = Get-ServerPids
    $recycled = ($after | Where-Object { $before -contains $_ }).Count -eq 0
    Write-Host ("  exit={0} {1}s / 常駐サーバ PID: {2}" -f $r2.Exit, $r2.Sec, ($after -join ','))
    Write-Host ''

    $okDev = (Read-Host '新しく選んだデバイスから鳴ったか？ [y/n]').Trim().ToLower() -eq 'y'

    Write-Host ''
    Write-Host '=== 判定 ===' -ForegroundColor Yellow
    if (-not $stillAlive) {
        Write-Host '  INCONCLUSIVE: 切替時点でサーバが既に終了していた。' -ForegroundColor DarkYellow
        Write-Host '  ID 比較ではなくデバイス消失で検知された可能性がある。' -ForegroundColor DarkYellow
        Write-Host '  旧デバイスの接続を保ったまま既定出力だけを変える形で再試行すること。' -ForegroundColor DarkYellow
    }
    elseif ($recycled -and $okDev -and $r2.Exit -eq 0) {
        Write-Host '  PASS: サーバ生存中の既定デバイス変更を ID 比較で検知し、新デバイスで鳴り直した。' -ForegroundColor Green
    }
    else {
        Write-Host ("  FAIL: recycled={0} newDevice={1} exit={2}" -f $recycled, $okDev, $r2.Exit) -ForegroundColor Red
    }
}
finally {
    Get-Process minply -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    if ($hadCfg) { Set-Content -Path $localCfg -Value $savedCfg -NoNewline }
    else { Remove-Item $localCfg -ErrorAction SilentlyContinue }
    Write-Host ''
    Write-Host '設定と一時ファイルを元に戻した。' -ForegroundColor DarkGray
}
