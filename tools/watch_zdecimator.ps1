# 监视减面大师的 zdecimator.exe：记录命令行、耗时、平均用了几个核、峰值内存，
# 并在它运行期间把命令行里提到的文件（GoZ、参数文件）复制一份，方便之后离线复现。
# 用法：powershell -ExecutionPolicy Bypass -File watch_zdecimator.ps1 [-Minutes 120]
param([int]$Minutes = 120, [string]$Out = "$PSScriptRoot\..\bench\dm_capture")

New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out 'watch.log'
function L($s) { $line = (Get-Date -Format 'HH:mm:ss.fff') + '  ' + $s; Add-Content -Encoding utf8 $log $line; Write-Output $line }

$cores = [Environment]::ProcessorCount
L "开始监视 zdecimator.exe（逻辑处理器 $cores 个，最长 $Minutes 分钟）"
$deadline = (Get-Date).AddMinutes($Minutes)
$seen = @{}
while ((Get-Date) -lt $deadline) {
    foreach ($p in Get-Process zdecimator -ErrorAction SilentlyContinue) {
        if ($seen.ContainsKey($p.Id)) { continue }
        $seen[$p.Id] = $true
        $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$($p.Id)"
        $cmd = $cim.CommandLine
        $run = Join-Path $Out ("run_{0}_{1}" -f (Get-Date -Format 'MMdd_HHmmss'), $p.Id)
        New-Item -ItemType Directory -Force $run | Out-Null
        L "[$($p.Id)] 启动：$cmd"
        # 复制命令行里出现的现有文件，以及同目录下的 .zpr/.GoZ
        $paths = [regex]::Matches($cmd, '"([^"]+)"|(\S+)') | ForEach-Object { if ($_.Groups[1].Success) { $_.Groups[1].Value } else { $_.Groups[2].Value } }
        $dirs = @{}
        foreach ($a in $paths) { if (Test-Path -LiteralPath $a -PathType Leaf) { Copy-Item -LiteralPath $a $run -ErrorAction SilentlyContinue; $dirs[(Split-Path $a)] = 1 } }
        foreach ($d in $dirs.Keys) { Get-ChildItem -LiteralPath $d -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.zpr','.GoZ','.txt' -and $_.Length -lt 2GB } | Copy-Item -Destination $run -ErrorAction SilentlyContinue }
        $t0 = Get-Date; $cpu0 = $p.TotalProcessorTime; $peak = 0; $maxThreads = 0; $maxInst = 0.0
        $lastT = $t0; $lastCpu = $cpu0
        while (-not $p.HasExited) {
            Start-Sleep -Milliseconds 250
            try { $p.Refresh() } catch { break }
            if ($p.HasExited) { break }
            $now = Get-Date; $c = $p.TotalProcessorTime
            $inst = ($c - $lastCpu).TotalSeconds / [math]::Max(0.001, ($now - $lastT).TotalSeconds)
            if ($inst -gt $maxInst) { $maxInst = $inst }
            $lastT = $now; $lastCpu = $c
            if ($p.PeakWorkingSet64 -gt $peak) { $peak = $p.PeakWorkingSet64 }
            if ($p.Threads.Count -gt $maxThreads) { $maxThreads = $p.Threads.Count }
            $cpuTotal = $c
        }
        $wall = ((Get-Date) - $t0).TotalSeconds
        $cpuSec = if ($cpuTotal) { $cpuTotal.TotalSeconds } else { 0 }
        L ("[{0}] 结束：耗时 {1:N2} 秒，CPU 时间 {2:N2} 秒，平均 {3:N2} 核，瞬时最高 {4:N1} 核，线程最多 {5} 个，峰值内存 {6:N0} MB" -f $p.Id, $wall, $cpuSec, ($cpuSec / [math]::Max(0.001, $wall)), $maxInst, $maxThreads, ($peak / 1MB))
    }
    Start-Sleep -Milliseconds 100
}
L "监视结束"
