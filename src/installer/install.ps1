# KM减面大王 安装 / 卸载（ZBrush 2024 / 2026）
# 用法：双击“安装.bat”或“卸载.bat”。命令行：
#   install.ps1                         列出找到的 ZBrush 插件目录，选择后安装
#   install.ps1 -Target <目录> -Yes     装到指定目录（ZBrush 文件夹、ZStartup 或 ZPlugs64 都可以），不提问
#   install.ps1 -Uninstall              卸载
#   install.ps1 -ListOnly               只列出找到的插件目录
param(
    [string]$Target = "",
    [switch]$Yes,
    [switch]$Uninstall,
    [switch]$ListOnly,
    [switch]$NoElevate,
    [switch]$Force      # 开发测试用：不检查 ZBrush 是否在运行、不结束减面程序
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Files = Join-Path $Here 'files'

function Say([string]$t, [string]$c = 'Gray') { Write-Host $t -ForegroundColor $c }
function Pause-End { if (-not $Yes) { Read-Host '按回车键关闭' | Out-Null } }

# ---- 管理员权限：ZBrush 装在 Program Files 里时需要
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $NoElevate -and -not $Yes -and -not $ListOnly) {
    try {
        $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"")
        if ($Uninstall) { $a += '-Uninstall' }
        Start-Process powershell -Verb RunAs -ArgumentList $a
        exit
    }
    catch { Say '没有获得管理员权限，以普通权限继续（ZBrush 装在 Program Files 里时可能装不进去）' Yellow }
}

# ---- 找 ZBrush 的插件目录（...\ZStartup\ZPlugs64）
# 插件目录 -> 用它的 ZBrush 2026 版本（2026 的资源目录可能被几个版本共用）
$script:Used = @{}

function Add-Found($list, [string]$z, [string]$who) {
    if (-not (Test-Path -LiteralPath $z)) { return }
    $full = (Resolve-Path -LiteralPath $z).Path.TrimEnd('\')
    if ($who) {
        if (-not $script:Used.ContainsKey($full)) { $script:Used[$full] = New-Object System.Collections.ArrayList }
        if (-not $script:Used[$full].Contains($who)) { [void]$script:Used[$full].Add($who) }
    }
    foreach ($x in $list) { if ($x -ieq $full) { return } }
    [void]$list.Add($full)
}

function Find-Targets {
    $found = New-Object System.Collections.ArrayList
    $roots = New-Object System.Collections.ArrayList
    foreach ($p in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, (Join-Path $env:PUBLIC 'Documents'), [Environment]::GetFolderPath('MyDocuments'))) {
        if ($p -and (Test-Path -LiteralPath $p)) { [void]$roots.Add($p) }
    }
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if ($d.DriveType -eq 'Fixed' -and $d.IsReady) { [void]$roots.Add($d.RootDirectory.FullName) }
    }
    foreach ($r in ($roots | Select-Object -Unique)) {
        foreach ($a in @(Get-ChildItem -LiteralPath $r -Directory -ErrorAction SilentlyContinue)) {
            if ($a.Name -notmatch 'zbrush|pixologic|maxon') { continue }
            $cands = @($a) + @(Get-ChildItem -LiteralPath $a.FullName -Directory -ErrorAction SilentlyContinue)
            foreach ($c in $cands) { Add-Found $found (Join-Path $c.FullName 'ZStartup\ZPlugs64') '' }
        }
    }
    # ZBrush 2026：插件在用户资源目录里。默认是 %APPDATA%\Maxon\ZBrush <版本>_<编号>\，
    # 用户改过位置时写在那里的 userassets_path.txt，这时 AppData 下的那份不会被读取
    $maxon = Join-Path $env:APPDATA 'Maxon'
    foreach ($m in @(Get-ChildItem -LiteralPath $maxon -Directory -ErrorAction SilentlyContinue)) {
        if ($m.Name -notmatch 'zbrush') { continue }
        $who = ($m.Name -replace '_[0-9A-Fa-f]{6,}$', '') -replace '^(?i)zbrush\s*', 'ZBrush '
        $p = ''
        $ua = Join-Path $m.FullName 'userassets_path.txt'
        if (Test-Path -LiteralPath $ua) { $p = ([IO.File]::ReadAllText($ua)).Trim().Trim([char]0) }
        if ($p) { Add-Found $found (Join-Path $p 'ZStartup\ZPlugs64') $who }
        else { Add-Found $found (Join-Path $m.FullName 'ZStartup\ZPlugs64') $who }
    }
    return $found
}

# 给用户看的说明：版本（由 ZBrush.exe 或 2026 资源目录的 installpath.txt 判断）、是否已经装过
function Describe([string]$zp) {
    $zb = Split-Path (Split-Path $zp -Parent) -Parent
    $ver = ''
    $exe = Join-Path $zb 'ZBrush.exe'
    $ip = Join-Path $zb 'installpath.txt'
    if (-not (Test-Path -LiteralPath $exe) -and (Test-Path -LiteralPath $ip)) {
        $prog = ([IO.File]::ReadAllText($ip)).Trim().Trim([char]0)
        if ($prog) { $exe = Join-Path $prog 'ZBrush.exe' }
    }
    if (Test-Path -LiteralPath $exe) { $vi = (Get-Item -LiteralPath $exe).VersionInfo; $ver = '{0}.{1}.{2}' -f $vi.ProductMajorPart, $vi.ProductMinorPart, $vi.ProductBuildPart }
    if (-not $ver -and $zp -match '(20\d\d(\.\d+)*)') { $ver = $matches[1] }
    $s = if ($ver) { "ZBrush $ver" } else { '版本未知' }
    $key = $zp.TrimEnd('\')
    if ($script:Used.ContainsKey($key)) { $s = ($script:Used[$key] -join '、') + ' 的资源目录' }
    if (Test-Path -LiteralPath (Join-Path $zp 'KMDecimate.txt')) { $s += '，已装过 KM减面大王' }
    return $s
}

# 用户输入的路径：ZBrush 文件夹、ZStartup 或 ZPlugs64 都可以
function Normalize-Target([string]$p) {
    $p = $p.Trim().Trim('"')
    if (-not $p) { return $null }
    foreach ($c in @($p, (Join-Path $p 'ZPlugs64'), (Join-Path $p 'ZStartup\ZPlugs64'))) {
        if ((Test-Path -LiteralPath $c) -and ((Split-Path $c -Leaf) -eq 'ZPlugs64')) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return $null
}

function Copy-Tree([string]$src, [string]$dst) {
    $null = robocopy $src $dst /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP
    if ($LASTEXITCODE -ge 8) { throw "复制失败：$src -> $dst（robocopy 返回 $LASTEXITCODE）" }
}

function Backup([string]$zp) {
    $old = @('KMDecimate.txt', 'KMDecimate.zsc', 'KMDecimate') | Where-Object { Test-Path -LiteralPath (Join-Path $zp $_) }
    if ($old.Count -eq 0) { return $null }
    $bk = Join-Path (Split-Path (Split-Path $zp -Parent) -Parent) ('KMDecimate备份_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
    New-Item -ItemType Directory -Force -Path $bk | Out-Null
    foreach ($n in $old) {
        $s = Join-Path $zp $n
        if (Test-Path -LiteralPath $s -PathType Container) { Copy-Tree $s (Join-Path $bk $n) } else { Copy-Item -LiteralPath $s -Destination $bk -Force }
    }
    return $bk
}

function Install-To([string]$zp) {
    Say ''
    Say "==== 安装到 $zp（$(Describe $zp)）" Cyan
    $bk = Backup $zp
    if ($bk) { Say "  原来的 KM减面大王已备份到 $bk" }
    Copy-Tree (Join-Path $Files 'KMDecimate') (Join-Path $zp 'KMDecimate')
    Copy-Item -LiteralPath (Join-Path $Files 'KMDecimate.txt') -Destination $zp -Force
    # 让 ZBrush 下次启动时自动编译：.txt 改成现在的时间；第一次安装时放一个时间很早的占位 .zsc
    (Get-Item -LiteralPath (Join-Path $zp 'KMDecimate.txt')).LastWriteTime = Get-Date
    $zsc = Join-Path $zp 'KMDecimate.zsc'
    if (-not (Test-Path -LiteralPath $zsc)) {
        Copy-Item -LiteralPath (Join-Path $Files 'placeholder\KMDecimate.zsc') -Destination $zsc
        (Get-Item -LiteralPath $zsc).LastWriteTime = Get-Date '2020-01-01'
    }
    # ZBrush 在 Program Files 里：编译时要改写 KMDecimate.zsc，给普通用户写权限
    $pf = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }
    foreach ($p in $pf) {
        if ($zp -like "$p*") {
            if ($isAdmin) {
                $null = icacls $zsc /grant '*S-1-5-32-545:M' /C /Q
                $null = icacls (Join-Path $zp 'KMDecimate') /grant '*S-1-5-32-545:(OI)(CI)M' /T /C /Q
                Say '  ZBrush 在 Program Files 里：已给 KM减面大王的文件开放写权限'
            }
            else { Say '  ZBrush 在 Program Files 里，但没有管理员权限：ZBrush 可能没法编译插件，请右键“安装.bat”以管理员身份运行' Yellow }
        }
    }
    Say '  完成' Green
}

function Uninstall-From([string]$zp) {
    Say ''
    Say "==== 从 $zp 卸载" Cyan
    if (-not (Test-Path -LiteralPath (Join-Path $zp 'KMDecimate.txt')) -and -not (Test-Path -LiteralPath (Join-Path $zp 'KMDecimate'))) { Say '  这里没有装 KM减面大王' Yellow; return }
    $bk = Backup $zp
    if ($bk) { Say "  删除前已备份到 $bk" }
    foreach ($n in @('KMDecimate.txt', 'KMDecimate.zsc')) { $f = Join-Path $zp $n; if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }
    $d = Join-Path $zp 'KMDecimate'
    if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
    Say '  已卸载' Green
}

# ---- 主流程
$what = if ($Uninstall) { '卸载' } else { '安装' }
Say "KM减面大王 {{VER}} $what（多线程减面 / 重拓扑并投射，ZBrush 2024 / 2026）" Cyan
if ($Target) { $targets = @((Normalize-Target $Target)) | Where-Object { $_ } }
else { Say '正在查找 ZBrush 的插件目录…'; $targets = @(Find-Targets) }
if ($ListOnly) { foreach ($t in $targets) { "$t  ($(Describe $t))" }; exit }

if (-not $Target) {
    Say ''
    for ($i = 0; $i -lt $targets.Count; $i++) { Say ("  [{0}] {1}  （{2}）" -f ($i + 1), $targets[$i], (Describe $targets[$i])) }
    if ($targets.Count -eq 0) { Say '  没有自动找到 ZBrush，请手动输入路径。' Yellow }
    Say '  [M] 手动输入路径（ZBrush 文件夹、ZStartup 或 ZPlugs64 都可以；ZBrush 2026 填它的资源目录，就是里面有 ZStartup 的那个）'
    $sel = Read-Host "选择要${what}到哪里（可以填多个，用逗号分隔，例如 1,2）"
    $chosen = New-Object System.Collections.ArrayList
    foreach ($s in ($sel -split '[,，\s]+' | Where-Object { $_ })) {
        if ($s -match '^[mM]$') {
            $p = Normalize-Target (Read-Host '输入路径')
            if ($p) { [void]$chosen.Add($p) } else { Say '  这个路径下找不到 ZStartup\ZPlugs64。' Red }
        }
        elseif ($s -match '^\d+$' -and [int]$s -ge 1 -and [int]$s -le $targets.Count) { [void]$chosen.Add($targets[[int]$s - 1]) }
    }
    $targets = @($chosen)
}
if ($targets.Count -eq 0) { Say "没有选择${what}位置，已退出。" Red; Pause-End; exit 1 }

if (-not $Force -and (Get-Process ZBrush -ErrorAction SilentlyContinue)) {
    Say 'ZBrush 正在运行：请先保存并关闭 ZBrush。' Red
    if ($Yes) { exit 1 }
    Read-Host '关闭后按回车继续' | Out-Null
    if (Get-Process ZBrush -ErrorAction SilentlyContinue) { Say 'ZBrush 还在运行，已退出。' Red; Pause-End; exit 1 }
}
# 减面程序还在运行时它的文件被占用，先结束它
if (-not $Force) { Get-Process KMDecimate -ErrorAction SilentlyContinue | Stop-Process -Force }
Start-Sleep -Milliseconds 300

$ok = $true
foreach ($t in $targets) {
    try { if ($Uninstall) { Uninstall-From $t } else { Install-To $t } }
    catch { $ok = $false; Say ("  失败：" + $_.Exception.Message) Red }
}

Say ''
if ($ok -and -not $Uninstall) {
    Say '全部完成。打开 ZBrush，第一次启动会自动编译插件，稍等一下；之后 Zplugin 菜单里会出现 KM Decimate（按钮是中文）。' Green
    Say '如果没出现：ZScript > Load，选 ZPlugs64 里的 KMDecimate.txt 编译一次（会清空画布，模型还在 Tool 面板里，选中后按 T 回来）。' Gray
    Say '用法见“使用说明.txt”。' Gray
}
elseif ($ok) {
    Say '卸载完成。设置和临时文件还留在 C:\Users\Public\KMDecimate\，不需要的话可以手动删除。' Green
}
Pause-End
