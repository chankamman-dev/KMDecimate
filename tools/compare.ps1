# 和减面大师对比：给原模型 OBJ 和减面大师结果 OBJ，自动减到同样面数，量两边误差，出两张同色阶的热力图。
# 用法：& compare.ps1 原模型.obj 减面大师结果.obj
param([Parameter(Mandatory)][string]$Orig, [Parameter(Mandatory)][string]$DM)
$k = "$PSScriptRoot\..\bin"
$base = [IO.Path]::Combine([IO.Path]::GetDirectoryName($DM), [IO.Path]::GetFileNameWithoutExtension($DM))
$ours = "${base}_km.obj"

$d1 = & "$k\meshdiff.exe" $Orig $DM
$dmTris = [int]([regex]::Match(($d1 -join "`n"), '结果 (\d+) 三角形').Groups[1].Value)
$dmDiff = ($d1 | Select-String DIFF).ToString()
$p99 = [double]([regex]::Match($dmDiff, 'p99=([\d.e+-]+)').Groups[1].Value)

$run = & "$k\kmdec.exe" $Orig $ours --tris $dmTris
$run | Out-Host
$t = ($run | Select-String TIMING).ToString()
$d2 = & "$k\meshdiff.exe" $Orig $ours

# 同一个色阶出热力图（红 = 减面大师 99 分位误差的 1.5 倍）
$hm = $p99 * 1.5
& "$k\meshdiff.exe" $Orig $DM --heat "${base}_heat_dm.obj" --heatmax $hm | Select-String 热力图
& "$k\meshdiff.exe" $Orig $ours --heat "${base}_heat_km.obj" --heatmax $hm | Select-String 热力图

"`n== 减面大师 =="; $d1 | Select-Object -Skip 1 -First 2
"`n== KMDecimate =="; $d2 | Select-Object -Skip 1 -First 2
"`n$t"
