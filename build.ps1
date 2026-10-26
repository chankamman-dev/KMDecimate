# 用本机 WinLibs g++ 编译原型（静态链接，不依赖额外 DLL）
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
New-Item -ItemType Directory -Force "$root\bin" | Out-Null
$flags = @('-std=c++20', '-O3', '-march=native', '-municode', '-static', '-pthread', '-Wall', '-Wno-sign-compare')
foreach ($t in 'kmdec', 'meshdiff', 'meshgen') {
    & g++ @flags "$root\src\$t.cpp" -o "$root\bin\$t.exe"
    if ($LASTEXITCODE -ne 0) { throw "编译 $t 失败" }
}
# ZBrush 插件用的无控制台窗口版本
& g++ @flags -mwindows "$root\src\kmdec.cpp" -o "$root\bin\KMDecimate.exe"
if ($LASTEXITCODE -ne 0) { throw "编译 KMDecimate 失败" }
"编译完成：$root\bin"
