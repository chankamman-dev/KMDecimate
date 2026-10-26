# KMDecimate（KM 减面）原型

目标：比 ZBrush 自带的减面大师（Decimation Master）更快的减面工具。现阶段只是一个命令行原型，用来和减面大师比速度和质量，还没有接进 ZBrush。

## 减面大师是怎么工作的（从安装文件里看到的）

- `ZPlugs64\DecimationMasterData\DecimationMasterDLL.dll` 把模型存成 GoZ 文件，再启动外部程序 `ZPlugs64\zdec\zdecimator.exe` 计算（导出 `LaunchDecimationMasterAppAndWait`、`GetDefaultNumThreads`）。
- 预处理就是"Build progressive mesh"（建渐进网格），结果写进 `VSplitList.data`（顶点拆分表）。之后选百分比，只是把这张表回放到某个位置（"Decimate progressive mesh"），所以很快。
- 误差度量是二次误差（`PM_QUADRIC3D_ID`），Polypaint 用颜色二次误差（`GPM_COLOR_QUADRIC_ID`）。有多线程模式 `doDecimation_LM_MT`，也有省内存的单线程模式 `MEMSPEED_TRADEOFF_LOW_MEM (SingleThread)`。实际用了几个核，要跑一次 `tools\watch_zdecimator.ps1` 才知道。

## 原型的做法

`src\decimate.h`：多线程 QEM 边折叠。

1. 按包围盒把三角形切成大约 线程数×6 块，块交界的顶点锁住，各块各自建堆、并行折叠。
2. 所有块共用同一个误差阈值，一轮一轮往上抬，所以整体仍然是"先删误差最小的"，细节多的地方留得多。
3. 每一遍最多减到当前面数的 1/4，下一遍把网格错开半格重新切块，上一遍的接缝就落到块内部被正常处理。块数会随网格变小自动减少，保证接缝点只占这一遍目标的一小部分（太多会逼着块内部削过头，这是第一版的 bug）。
4. `--threads 1` 是经典的单线程全局堆 QEM，用作质量对照。

## 目前的测试结果（i7-13700KF，24 线程）

| 模型 | 比例 | 多线程 | 单线程 | 平均误差（多线程 / 单线程） |
|---|---|---|---|---|
| 部件 3.stl，188 万面 | 10% | 0.58 秒 | 10.2 秒 | 1.876e-4 / 1.861e-4 |
| 同上 | 2% | 0.7–1.3 秒 | 11.7 秒 | 4.53e-4 / 4.49e-4 |
| 同上 | 0.5% | 0.77 秒 | 11.1 秒 | 1.423e-3 / 1.421e-3 |
| 细分后加起伏，750 万面 | 10% | 2.2 秒 | 54.7 秒 | 1.007e-3 / 1.002e-3 |
| 同上 | 1% | 2.3 秒 | 59.0 秒 | 3.004e-3 / 2.984e-3 |

多线程和单线程的误差差别在 1% 以内。读 289MB 的 OBJ 约 0.5 秒。

还不支持：UV 不保留（OBJ 里的 vt 被忽略）；Polypaint 只是沿边插值，没有做颜色误差；polygroup 边界默认不保护（`--groups 10` 可以打开）；遮罩不保留。

## ZBrush 插件（0.1）

面板：Zplugin > KM Decimate（按钮显示中文）。
- **减面**：默认先复制当前子工具，再在副本上删掉低细分级、减面，结果放在原物体下面，名字是 `原名_KM百分比`。弹窗显示导出、计算、导入各用了几秒。
- **保留百分比**：0.1–100。**保留组边界**：默认开。**保留原物体**：关掉时直接减当前子工具（有细分级时仍然复制，因为原地删低级别没法撤销）。
- 流程：ZScript 导出 GoZ 到 `C:\Users\Public\KMDecimate\work\in.GoZ` → `KMDecimate.exe job.txt` → 写 `out.GoZ` + `status.txt` + `note.txt` → ZScript 导入。日志 `work\log.txt`。
- 组装与安装：先 `build.ps1`，再 `py -3 tools/package.py --install`（默认装到 `F:\Zbrush Assest\ZStartup\ZPlugs64` 和 `F:\art software\zbrush 2024.0.4\ZStartup\ZPlugs64`）。改 ZScript 后 package.py 会先跑 `tools/zscript_lint.py`。
- 第一次需要在 ZBrush 里 ZScript > Load 选 `ZPlugs64\KMDecimate.txt`，之后 ZBrush 启动会自动编译。

## 和减面大师对比：测试步骤

1. 在 ZBrush 里选中要测的 SubTool（最高细分级），Tool > Export 导出 OBJ，比如存到 `G:\ai liabary\KMDecimate\bench\input\测试.obj`。
2. 确认 `tools\watch_zdecimator.ps1` 正在运行（它会记录 zdecimator.exe 的耗时、用了几个核、峰值内存，日志在 `bench\dm_capture\watch.log`）。
3. 在 ZBrush 里用减面大师：Pre-process Current，然后减到想测的比例（比如 10%、2%）。记下预处理和减面各用了多久，以及勾了哪些选项（Keep UVs、Keep Groups 等）。
4. 把减面大师的结果也导出 OBJ，比如 `bench\input\测试_dm10.obj`。
5. 运行对比（会减到同样面数，量两边误差，并出两张同色阶的热力图 `*_heat_dm.obj` 和 `*_heat_km.obj`，导回 ZBrush 能直接看到误差分布）：

```powershell
& 'G:\ai liabary\KMDecimate\tools\compare.ps1' 'G:\ai liabary\KMDecimate\bench\input\测试.obj' 'G:\ai liabary\KMDecimate\bench\input\测试_dm10.obj'
```

## 文件

- `src\meshio.h`：OBJ（含 `#MRGB`、`g`）、GoZ、二进制 STL 的读写
- `src\decimate.h`：减面核心
- `src\kmdec.cpp`：命令行 `kmdec 输入 输出 [--percent P | --tris N | --verts N] [--threads N]`
- `src\meshdiff.cpp`：双向表面距离 + 热力图
- `src\meshgen.cpp`：造大测试模型
- `build.ps1`：用本机 WinLibs g++ 16 编译到 `bin\`
