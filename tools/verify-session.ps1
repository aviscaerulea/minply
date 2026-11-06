# minply --session 常駐モードの聴感検証スクリプト
#
# 常駐モードは音声デバイスの実挙動に依存するため自動テストができない。
# 機械判定できない 3 点をユーザの耳で確認する。
#   A: ウォーム再生で冒頭が欠けていないか
#   B: 異なるセッション ID が並行再生されるか
#   C: 既定デバイス切替で常駐が畳まれ、新デバイスで鳴り直すか
#
# テスト A は Bluetooth レシーバ（Shokz 等）で実施すること。有線では元々スリープ
# しないため、通っても冒頭欠け防止の検証にならない。
#
# 実行: pwsh -ExecutionPolicy Bypass -File tools\verify-session.ps1

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root 'out\minply.exe'
if (-not (Test-Path $exe)) { throw "minply.exe が見つからない（先に task build を実行する）: $exe" }

# 音源はリポジトリ外の一時ディレクトリへ出す
# .NET API はプロセスのカレントディレクトリを見るため、パスは常に絶対で扱う
$work = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $work | Out-Null
$wavBeeps = Join-Path $work 'beeps.wav'
$wavLow   = Join-Path $work 'low.wav'
$wavHigh  = Join-Path $work 'high.wav'

# 16bit PCM モノラル WAV を書き出す
# segments は @{ Freq = <Hz>; Sec = <秒> } の配列。Freq = 0 は無音を表す
function New-ToneWav {
    param(
        [string]$Path,
        [array]$Segments,
        [int]$Rate = 48000,
        [double]$Amp = 0.5
    )

    $fadeLen = [int]($Rate * 0.005)   # クリック音防止のフェード長

    # 総サンプル数を先に確定して固定長配列へ書く（List への逐次追加より大幅に速い）
    $total = 0
    foreach ($seg in $Segments) { $total += [int]($Rate * $seg.Sec) }
    $samples = New-Object int16[] $total

    $pos = 0
    foreach ($seg in $Segments) {
        $count = [int]($Rate * $seg.Sec)
        if ($seg.Freq -le 0) { $pos += $count; continue }   # 無音は初期値 0 のまま
        $step = 2 * [math]::PI * $seg.Freq / $Rate
        for ($i = 0; $i -lt $count; $i++) {
            $v = $Amp * [math]::Sin($step * $i)
            # 各セグメントの端をフェードして波形の不連続を消す
            if ($i -lt $fadeLen) { $v *= $i / $fadeLen }
            $tail = $count - 1 - $i
            if ($tail -lt $fadeLen) { $v *= $tail / $fadeLen }
            $samples[$pos + $i] = [int16][math]::Round($v * 32767)
        }
        $pos += $count
    }

    $dataSize = $samples.Length * 2
    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter($fs)
    try {
        $bw.Write([char[]]'RIFF'); $bw.Write([uint32](36 + $dataSize)); $bw.Write([char[]]'WAVE')
        $bw.Write([char[]]'fmt '); $bw.Write([uint32]16)
        $bw.Write([uint16]1); $bw.Write([uint16]1)
        $bw.Write([uint32]$Rate); $bw.Write([uint32]($Rate * 2))
        $bw.Write([uint16]2); $bw.Write([uint16]16)
        $bw.Write([char[]]'data'); $bw.Write([uint32]$dataSize)
        foreach ($s in $samples) { $bw.Write($s) }
    }
    finally { $bw.Dispose(); $fs.Dispose() }
}

# minply を起動し、そのプロセスの終了だけを待つ
# WINDOWS サブシステムの exe は通常起動では待てないため WaitForExit を明示する
# パラメータ名に Args は使えない（PowerShell の自動変数と衝突して束縛が壊れる）
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

# out\minply.exe のプロセスだけを対象にする
# プロセス名で選ぶと、別の場所に常駐しているユーザ自身のトレイまで巻き込む
function Get-ServerPids {
    return @(Get-Process minply -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Select-Object -ExpandProperty Id)
}

function Stop-Servers {
    Get-Process minply -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force
    Start-Sleep -Milliseconds 800
}

function Read-YesNo {
    param([string]$Prompt)
    while ($true) {
        $a = (Read-Host "$Prompt [y/n]").Trim().ToLower()
        if ($a -eq 'y') { return $true }
        if ($a -eq 'n') { return $false }
    }
}

$results = [ordered]@{}

try {
    # --- テスト音源の生成 -------------------------------------------------

    # 冒頭検出用: 1 発目だけ低音（600Hz）、以降 4 発は高音（1200Hz）
    # 冒頭が欠けると「低いピッ」が聞こえず、ビープ数も 5 未満になる
    $beepSegs = @(@{ Freq = 600; Sec = 0.15 }, @{ Freq = 0; Sec = 0.15 })
    for ($i = 0; $i -lt 4; $i++) {
        $beepSegs += @{ Freq = 1200; Sec = 0.15 }
        $beepSegs += @{ Freq = 0;    Sec = 0.15 }
    }

    Write-Host 'テスト音源を生成中...' -ForegroundColor Cyan
    New-ToneWav -Path $wavBeeps -Segments $beepSegs
    New-ToneWav -Path $wavLow   -Segments @(@{ Freq = 300;  Sec = 2.5 })
    New-ToneWav -Path $wavHigh  -Segments @(@{ Freq = 1500; Sec = 2.5 })
    foreach ($w in @($wavBeeps, $wavLow, $wavHigh)) {
        if (-not (Test-Path $w)) { throw "音源の生成に失敗した: $w" }
        Write-Host ("  {0} ({1:N0} bytes)" -f (Split-Path $w -Leaf), (Get-Item $w).Length) -ForegroundColor DarkGray
    }
    Write-Host ''

    # --- テスト A: ウォーム再生の冒頭欠け ----------------------------------

    Write-Host '=== テスト A: ウォーム再生で冒頭が欠けていないか ===' -ForegroundColor Yellow
    Write-Host 'Bluetooth レシーバを既定の出力先にし、直前 30 秒は無音にしておくこと。'
    Write-Host '（寝た状態から始めないとコールドとウォームの対比が成立しない）'
    Write-Host '合図音を 3 回鳴らす。毎回「低いピッ」で始まり、計 5 発鳴るのが正常。'
    Read-Host '準備ができたら Enter'

    Stop-Servers
    $a1 = Invoke-Minply @('--session', 'verifyA', $wavBeeps)
    Write-Host ("  1 回目 コールド : exit={0} {1}s" -f $a1.Exit, $a1.Sec)
    Start-Sleep -Seconds 2
    $a2 = Invoke-Minply @('--session', 'verifyA', $wavBeeps)
    Write-Host ("  2 回目 ウォーム : exit={0} {1}s" -f $a2.Exit, $a2.Sec)
    Start-Sleep -Seconds 2
    $a3 = Invoke-Minply @('--session', 'verifyA', $wavBeeps)
    Write-Host ("  3 回目 ウォーム : exit={0} {1}s" -f $a3.Exit, $a3.Sec)
    Write-Host ''

    $okA1 = Read-YesNo '3 回とも「低いピッ」から始まったか？'
    $okA2 = Read-YesNo '3 回ともビープは 5 発聞こえたか？'
    $results['A: ウォーム冒頭欠けなし'] = if ($okA1 -and $okA2) { 'PASS' } else { 'FAIL' }
    Write-Host ''

    # --- テスト B: 異なるセッション ID の並行再生 ---------------------------

    Write-Host '=== テスト B: 異なるセッション ID が並行再生されるか ===' -ForegroundColor Yellow
    Write-Host '低音（ブー）と高音（ピー）を別 ID で同時に投げる。'
    Write-Host '重なって和音のように聞こえれば並行、順番に鳴れば直列。'
    Read-Host '準備ができたら Enter'

    Stop-Servers
    $t = Get-Date
    $x = Start-Process -FilePath $exe -ArgumentList @('--session', 'verifyB1', $wavLow)  -PassThru -NoNewWindow
    $y = Start-Process -FilePath $exe -ArgumentList @('--session', 'verifyB2', $wavHigh) -PassThru -NoNewWindow
    $x.WaitForExit(); $y.WaitForExit()
    Write-Host ("  exit={0},{1} / 合計 {2}s / サーバ {3} 個 (並行なら 2 個、約 4s 以内)" -f `
        $x.ExitCode, $y.ExitCode, [math]::Round(((Get-Date) - $t).TotalSeconds, 2), (Get-ServerPids).Count)
    Write-Host ''

    $okB = Read-YesNo '低音と高音が重なって聞こえたか？（順番に鳴ったなら n）'
    $results['B: 異なる ID の並行再生'] = if ($okB) { 'PASS' } else { 'FAIL' }
    Write-Host ''

    # 対照実験。同一 ID なら直列になり、B より明らかに長くかかる
    Write-Host '--- 参考: 同一 ID では直列になるはず ---' -ForegroundColor DarkGray
    Stop-Servers
    $t = Get-Date
    $x = Start-Process -FilePath $exe -ArgumentList @('--session', 'verifyB3', $wavLow)  -PassThru -NoNewWindow
    $y = Start-Process -FilePath $exe -ArgumentList @('--session', 'verifyB3', $wavHigh) -PassThru -NoNewWindow
    $x.WaitForExit(); $y.WaitForExit()
    Write-Host ("  exit={0},{1} / 合計 {2}s (直列なら B より明らかに長い)" -f `
        $x.ExitCode, $y.ExitCode, [math]::Round(((Get-Date) - $t).TotalSeconds, 2))
    $okB2 = Read-YesNo '低音と高音が順番に鳴ったか？（重なったなら n）'
    $results['B2: 同一 ID の直列再生'] = if ($okB2) { 'PASS' } else { 'FAIL' }
    Write-Host ''

    # --- テスト C: 既定デバイス切替 ----------------------------------------

    Write-Host '=== テスト C: 既定デバイス切替で常駐が畳まれ鳴り直すか ===' -ForegroundColor Yellow
    Write-Host 'サーバ生存中の切替を狙うなら verify-device-switch.ps1 を使うこと。'
    Write-Host '（linger 満了で自然終了すると、どの経路で検知したか判別できない）'
    Stop-Servers
    Read-Host '準備ができたら Enter'

    $c1 = Invoke-Minply @('--session', 'verifyC', $wavBeeps)
    $pidsBefore = Get-ServerPids
    Write-Host ("  再生: exit={0} {1}s / 常駐サーバ PID: {2}" -f $c1.Exit, $c1.Sec, ($pidsBefore -join ','))
    Read-Host '  どのデバイスから鳴ったか（例: 有線 / Shokz）' | Out-Null
    Write-Host ''
    Write-Host 'Windows の設定でサウンド出力先を別デバイスへ切り替えてくれ。' -ForegroundColor Cyan
    Read-Host '切り替えたら Enter'

    $c2 = Invoke-Minply @('--session', 'verifyC', $wavBeeps)
    $pidsAfter = Get-ServerPids
    Write-Host ("  再生: exit={0} {1}s / 常駐サーバ PID: {2}" -f $c2.Exit, $c2.Sec, ($pidsAfter -join ','))

    $recycled = ($pidsAfter | Where-Object { $pidsBefore -contains $_ }).Count -eq 0
    Write-Host ("  サーバ再起動: {0}" -f $(if ($recycled) { 'された（PID が変わった）' } else { 'されていない（PID 同一）' }))
    Write-Host ''

    $okC = Read-YesNo '切替後の再生は、新しく選んだデバイスから鳴ったか？'
    $results['C: デバイス切替で鳴り直す'] = if ($okC -and $recycled -and $c2.Exit -eq 0) { 'PASS' } else { 'FAIL' }
}
finally {
    Stop-Servers
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host '=== 結果 ===' -ForegroundColor Yellow
foreach ($k in $results.Keys) {
    $color = if ($results[$k] -eq 'PASS') { 'Green' } else { 'Red' }
    Write-Host ("  {0,-28} {1}" -f $k, $results[$k]) -ForegroundColor $color
}
Write-Host ''
if ($results.Values -contains 'FAIL') {
    Write-Host 'FAIL があった。上の出力をそのまま貼ってくれ。' -ForegroundColor Red
}
elseif ($results.Count -gt 0) {
    Write-Host '全項目 PASS。' -ForegroundColor Green
}
