# 对一个模型在几个比例下跑 多线程 / 单线程，记录耗时和误差
param([Parameter(Mandatory)][string]$In, [double[]]$Percents = @(10, 2, 0.5), [string]$Tag = 'm', [switch]$SkipSingle)
$k = "$PSScriptRoot\..\bin"; $b = "$PSScriptRoot\..\bench"
New-Item -ItemType Directory -Force $b | Out-Null
foreach ($p in $Percents) {
    $modes = @(@{n = 'mt'; a = @()})
    if (-not $SkipSingle) { $modes += @{n = 'st'; a = @('--threads', '1')} }
    foreach ($m in $modes) {
        $out = "$b\${Tag}_$($m.n)_$p.obj"
        $t = (& "$k\kmdec.exe" $In $out --percent $p @($m.a) | Select-String TIMING).ToString()
        $d = (& "$k\meshdiff.exe" $In $out | Select-String DIFF).ToString()
        $dec = [regex]::Match($t, 'decimate=([\d.]+)').Groups[1].Value
        $tris = [regex]::Match($t, 'out_tris=(\d+)').Groups[1].Value
        $mean = [regex]::Match($d, 'mean=([\d.e+-]+)').Groups[1].Value
        $rms = [regex]::Match($d, 'rms=([\d.e+-]+)').Groups[1].Value
        $hd = [regex]::Match($d, 'hausdorff=([\d.e+-]+)').Groups[1].Value
        '{0,6}%  {1}  decimate {2,7}s  tris {3,9}  mean {4}  rms {5}  hausdorff {6}' -f $p, $m.n, $dec, $tris, $mean, $rms, $hd
    }
}
