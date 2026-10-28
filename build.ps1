# 用本机 WinLibs g++ 编译原型（静态链接，不依赖额外 DLL）
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
New-Item -ItemType Directory -Force "$root\bin" | Out-Null
# x86-64-v2（2009 年以后的 CPU）：要发给别人用，不能用 -march=native
$flags = @('-std=c++20', '-O3', '-march=x86-64-v2', '-mtune=generic', '-municode', '-static', '-pthread', '-Wall', '-Wno-sign-compare')
# kmdec 的检查更新（src\update.h）要用 WinHTTP 和 ShellExecute
$libs = @('-lwinhttp', '-lshell32')
foreach ($t in 'kmdec', 'meshdiff', 'meshgen', 'meshshot') {
    & g++ @flags "$root\src\$t.cpp" -o "$root\bin\$t.exe" @libs
    if ($LASTEXITCODE -ne 0) { throw "编译 $t 失败" }
}
# ZBrush 插件用的无控制台窗口版本
& g++ @flags -mwindows "$root\src\kmdec.cpp" -o "$root\bin\KMDecimate.exe" @libs
if ($LASTEXITCODE -ne 0) { throw "编译 KMDecimate 失败" }
"编译完成：$root\bin"
