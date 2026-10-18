# 充电功率监控 PowerMon —— C++ 重写开发日志

> 项目周期：2026-10-10 23:40 ～ 2026-10-11 01:23
> 交付物：`D:\LE\AI\wbwrk\power-mon-cpp\PowerMon.exe`（单文件，桌面副本 `C:\Users\zhedu\Desktop\PowerMon.exe`）
> 源码：`D:\LE\AI\wbwrk\power-mon-cpp\main.cpp`（单文件，~700 行）

> **归档说明**：本文是当时的第一版开发记录，保留原样。其中「三维功耗公式」等描述已被
> 2026-10-17 的实测标定替换，现行模型见 [功率精度排查报告](功率精度排查报告.md)。
> 仓库中的 `src/main.cpp` 是标定后的版本（v1.2.0），并额外提供了 `-DPOWERMON_LITE` 精简构建。

---

## 一、背景与决策

### 1.1 为什么要重写

原工具是 2097 行 PowerShell 脚本（`ChargingPower.ps1`），存在三个核心问题：

| 问题 | 表现 |
|---|---|
| 代码臃肿 | 2000+ 行，WPF/WinForms + GDI 自绘混用，维护困难 |
| 依赖沉重 | 功耗/温度依赖 LibreHardwareMonitor（13MB 外部程序 + 手动开 Web Server） |
| 启动慢 | PowerShell 解释执行 + 自绘位图，打开要好几秒 |

### 1.2 技术选型

用户提出「C++ 还是 Python」，经对比后**用户拍板 C++**：

| 维度 | C++ | Python | C#（未采纳） |
|---|---|---|---|
| 打包 | 单 exe 470KB | PyInstaller 50MB+ | 单 exe 但依赖 LHM 库 |
| 启动 | 毫秒级 | 慢（解释器+导包） | 快 |
| 依赖 | 零 | 需打包 | .NET 运行时 |
| 开发量 | 中（本次约 700 行） | 低 | 低 |

**决策**：C++17 + Win32 API + GDI + WMI，纯静态链接，单文件交付。

### 1.3 需求变更

- ~~风扇转速~~（用户中途取消——该机器风扇由 EC 管理，读取需管理员 + 内核驱动，代价与收益不成正比）
- ✅ 新增：电源模式切换（平衡 / 卓越性能）
- ✅ 新增：系统总功耗显示

---

## 二、开发环境

| 项目 | 内容 |
|---|---|
| 编译器 | MinGW-w64 g++ 16.2.0（`D:\LE\AI\tools\mingw64\`，从 niXman builds 下载 103MB） |
| 编译命令 | `g++ -std=c++17 -O2 -municode -mwindows -static -o PowerMon.exe main.cpp app.res.o -lgdi32 -luser32 -lole32 -loleaut32 -ladvapi32 -luuid -lshell32 -lpdh -lshlwapi` |
| 图标资源 | `windres app.rc -O coff -o app.res.o`（复用旧版 app-light.ico） |
| 机器 | ThinkBook 16 G7 AHP（Ryzen 7 8745H + Radeon 780M），Win11 Build 26300 |

---

## 三、功能清单（最终交付版）

### 3.1 数据读取

| 数据 | 来源 | 说明 |
|---|---|---|
| 充电/放电功率 | WMI `root\wmi` → `BatteryStatus`（ChargeRate / DischargeRate） | 单位 mW → W |
| 电压 / 电流 | `BatteryStatus.Voltage`（mV）；电流 = 功率 ÷ 电压 | |
| 电量 / 满充容量 | `BatteryStatus.RemainingCapacity` + `BatteryFullChargedCapacity` | |
| 健康度 / 循环次数 | `BatteryStaticData.DesignedCapacity` + `BatteryCycleCount` | 每 30 秒刷新 |
| CPU 占用 | `GetSystemTimes` 差分（零开销，纯 Win32） | |
| GPU 占用 | WMI `root\cimv2` → `Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine`（engtype_3D 求和） | |
| SoC 温度 | PDH 计数器 `\Thermal Zone Information(*)\High Precision Temperature` | 0.1K → °C，取最大值 |
| 电源模式 | PowrProf API（动态加载）：PowerEnumerate / PowerReadFriendlyName / PowerGetActiveScheme / PowerSetActiveScheme | 支持隐藏方案发现 |

### 3.2 系统总功耗（三级模型，对齐旧版语义）

| 场景 | 算法 | 标记 |
|---|---|---|
| 电池放电中 | 放电功率（**实测**） | 无前缀 |
| 充电中 | 充电功率 + 系统估算 | `≈` |
| 已接通但无流动 | 系统估算 | `≈` |

系统估算公式（Ryzen 7 8745H 实机标定）：
```
SoC ≈ 17 W + CPU% × 0.20 + GPU% × 0.10    （上限 45 W）
```

### 3.3 界面

- 无边框卡片式设计：浅灰背景 + 白色圆角卡片
- 大数字功率（颜色区分：充电绿 / 放电橙 / 其他灰），无流动时直接显示总功耗
- 圆角电量条（低电量变红）
- 8 格指标网格（2 列 × 4 行）
- 自绘圆角按钮：当前激活方案显示绿色实心，点另一个即时切换
- 中文界面，DPI 自适应

---

## 四、踩坑记录（技术沉淀）

| # | 坑 | 现象 | 解决 |
|---|---|---|---|
| 1 | **msvcrt 的 `swprintf` 里 `%s` 是窄字符串** | 状态行中文变乱码 `ð]¥c□□5un` | 宽字符串必须用 `%ls` |
| 2 | MinGW 无 `_bstr_t` | 编译报错（MSVC 专有 comutil） | 自封装 BStr 结构（SysAllocString） |
| 3 | 电源方案枚举 API 名 | 记成 `PowerEnumerateGuids`（不存在） | 正确 API 是 `PowerEnumerate`，ACCESS_SCHEME=16 |
| 4 | `GetModuleHandle` 找不到未加载 DLL | powrprof 全部函数指针为 null | 改用 `LoadLibraryW("powrprof.dll")` |
| 5 | 注册表 GUID 键名不带大括号 | `CLSIDFromString` 全部失败，枚举为空 | 手写 `swscanf("%8x-%4x-...")` 解析 |
| 6 | **「卓越性能」方案被系统隐藏** | 未激活时 API 枚举不到 → 切不回去 | ① 注册表扫描发现（HKLM\...\PowerSchemes + SHLoadIndirectString 解析 MUI 名）② GUID 记忆到 `%APPDATA%\PowerMon\PowerMon.ini` |
| 7 | 沙箱拦 WScript.Shell COM | 无法程序化创建 .lnk | 桌面直接放 exe 本体（普通文件复制持久可靠） |
| 8 | Git Bash 下 powercfg 参数被 MSYS 转换 | `/getactivescheme` 被当成路径 | 一律用 PowerShell 执行 |
| 9 | MinGW 不可用 `#pragma comment(lib)` | 链接阶段缺库 | 全部写在命令行 `-l` 参数 |

其中 **6 号坑**是导致「按钮点了没反应」的直接原因，也是最值得沉淀的：
> Windows 的 Ultimate Performance 方案默认带 hidden 标志，未激活时 `PowerEnumerate` 看不见它。
> 唯一可靠的发现途径是注册表 `HKLM\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes\<GUID>`，
> FriendlyName 是 `@dll,-id,fallback` 形式的 MUI 引用，需用 `SHLoadIndirectString` 解析。

---

## 五、验证与测试

内置三种自检模式（无需人工点界面）：

| 模式 | 用途 | 结果 |
|---|---|---|
| `PowerMon.exe --console` | 控制台输出全部读数后退出 | ✅ 电压 16.555V / 79.6% / 健康 95.5% / 循环 70 / 温度 57.1°C 全部与实测吻合 |
| `PowerMon.exe --snapshot` | 显示窗口 4 秒 → 截图 BMP → 退出 | ✅ 用于 UI 走查（共 4 轮迭代） |
| `PowerMon.exe --click-test` | **BM_CLICK 模拟真实按钮点击**，验证「按钮 → WM_COMMAND → 切换」全链路 | ✅ `平衡 → 卓越性能 → 平衡` 往返切换全部成功 |

数据一致性抽查（与旧版截图对照）：电压 16.55V ✓、电量 79%/80% ✓、健康 95.5% ✓、循环 70 次 ✓、容量 67.78/71.0 Wh ✓、SoC 温度 57.1°C ✓。

---

## 六、旧版清理

| 操作 | 详情 |
|---|---|
| 备份 | `D:\LE\AI\wbwrk\_backup\charging-power-old-20261011.zip`（7.5MB，71 个文件） |
| 备份 | `_backup\旧桌面快捷方式-充电功率监控.lnk.bak`、`_backup\ChargingPower-便携版.zip` |
| 删除 | `D:\LE\AI\dshwork\charging-power\` 整个目录（含 LibreHardwareMonitor） |
| 删除 | 桌面 `充电功率监控.lnk`（旧快捷方式） |
| 替换 | 桌面新增 `PowerMon.exe`（新程序，双击即用） |

---

## 七、交付清单

| 文件 | 路径 | 说明 |
|---|---|---|
| 主程序 | `C:\Users\zhedu\Desktop\PowerMon.exe` | 桌面入口，双击即用（520KB） |
| 源码 | `D:\LE\AI\wbwrk\power-mon-cpp\main.cpp` | 单文件，含中文注释 |
| 图标 | `power-mon-cpp\app.ico` + `app.rc` + `app.res.o` | 资源编译链 |
| 配置 | `%APPDATA%\PowerMon\PowerMon.ini` | 自动生成（电源方案 GUID 记忆） |
| 备份 | `D:\LE\AI\wbwrk\_backup\*` | 旧版完整备份 |

## 八、已知限制与后续建议

1. **总功耗为粗估模型**：标定自本机（8745H + 780M），换机器误差会变大；如需精确值可重新引入 LibreHardwareMonitor 实测 SoC 功耗
2. **GPU 占用偶发 0.0%**：GPUEngine 计数器在部分时刻无实例，属系统计数器行为（旧版同样存在）
3. **更新方式**：修改源码后重编译，把 exe 复制到桌面覆盖即可（配置在 APPDATA，不受影响）
4. 若后续想加「自定义方案」（如节能模式），在 `ReadSchemes()` 里扩展匹配规则即可，架构已留好扩展点

---
*日志生成时间：2026-10-11 01:27*
