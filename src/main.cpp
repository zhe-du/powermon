// 充电功率监控 (PowerMon) v1.2.0
// 电池功率/电压/电流/电量/健康 + CPU/GPU 占用 + SoC 温度 + 系统总功耗
// 完整版另含电源模式切换（平衡 / 卓越性能）
// Win32 + GDI + WMI，单文件，无第三方依赖。MinGW-w64 编译。
//
// 两种构建（同一份源码）：
//   完整版  g++ ... -o PowerMon.exe       main.cpp   会读写 %APPDATA%\PowerMon\PowerMon.ini
//   精简版  g++ ... -DPOWERMON_LITE -o PowerMon-Lite.exe main.cpp
//          精简版不含电源模式切换相关代码：不加载 powrprof.dll、不写 ini、不显示方案按钮。

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define _WIN32_DCOM
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wbemidl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>

// MinGW: 手动定义 WMI GUID，不依赖 wbemuuid.lib
static const CLSID kCLSID_WbemLocator = { 0x4590F811,0x1D3A,0x11D0,{0x89,0x1F,0x00,0xAA,0x00,0x4B,0x2E,0x24} };
static const IID    kIID_IWbemLocator = { 0xDC12A687,0x737F,0x11CF,{0x88,0x4D,0x00,0xAA,0x00,0x4B,0x2E,0x24} };

// _bstr_t 替代（MinGW 无 comutil）
struct BStr {
    BSTR p;
    explicit BStr(const wchar_t* s) : p(SysAllocString(s)) {}
    ~BStr() { if (p) SysFreeString(p); }
    operator BSTR() const { return p; }
    BStr(const BStr&) = delete;
    BStr& operator=(const BStr&) = delete;
};

// ---------------------------------------------------------------------------
// 数据
// ---------------------------------------------------------------------------
struct Data {
    double chargeW = 0, dischargeW = 0;   // 充电/放电功率 W
    double powerW = 0;                    // 显示功率（充为正/放为负）
    double voltageV = 0, currentA = 0;
    double percent = -1;                  // 电量 %
    double health = -1;                   // 健康度 %
    long   cycles = -1;
    double fullWh = 0, designWh = 0;
    bool   online = false;
    bool   hasBattery = false;
    double cpuPct = -1;
    double gpuPct = -1;
    double socTempC = -999;
#ifndef POWERMON_LITE
    GUID   schemeBalanced{}, schemeUltimate{};
    GUID   schemeActive{};
    bool   hasBalanced = false, hasUltimate = false;
    wchar_t schemeActiveName[128] = L"";
#endif
};
static Data g;

// ---------------------------------------------------------------------------
// WMI（root\wmi）
// ---------------------------------------------------------------------------
static IWbemServices* g_wmi = nullptr;    // root\wmi（电池/温度）
static IWbemServices* g_wmi2 = nullptr;   // root\cimv2（GPU 占用）

static bool WmiInit() {
    HRESULT hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) return false;
    IWbemLocator* loc = nullptr;
    hr = CoCreateInstance(kCLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
        kIID_IWbemLocator, (void**)&loc);
    if (FAILED(hr)) return false;
    hr = loc->ConnectServer(BStr(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
        0, nullptr, nullptr, &g_wmi);
    if (FAILED(hr)) g_wmi = nullptr;
    hr = loc->ConnectServer(BStr(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr,
        0, nullptr, nullptr, &g_wmi2);
    if (FAILED(hr)) g_wmi2 = nullptr;
    loc->Release();
    if (!g_wmi && !g_wmi2) return false;
    // 设置代理安全级别，避免部分系统上查询被拒
    IWbemServices* svcs[2] = { g_wmi, g_wmi2 };
    for (int i = 0; i < 2; i++) {
        if (!svcs[i]) continue;
        IUnknown* unk = nullptr;
        if (SUCCEEDED(svcs[i]->QueryInterface(IID_IUnknown, (void**)&unk)) && unk) {
            CoSetProxyBlanket(unk, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
            unk->Release();
        }
    }
    return true;
}

static bool WmiQueryValue(const wchar_t* wql, const wchar_t* prop, double* out) {
    *out = 0;
    if (!g_wmi) return false;
    IEnumWbemClassObject* en = nullptr;
    HRESULT hr = g_wmi->ExecQuery(BStr(L"WQL"), BStr(wql),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    if (FAILED(hr)) return false;
    IWbemClassObject* obj = nullptr; ULONG ret = 0;
    bool ok = false;
    if (en->Next(WBEM_INFINITE, 1, &obj, &ret) == S_OK && ret) {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(obj->Get(prop, 0, &v, nullptr, nullptr)) && v.vt != VT_NULL) {
            switch (v.vt) {
            case VT_UI4: *out = (double)v.ulVal; ok = true; break;
            case VT_I4:  *out = (double)v.lVal;  ok = true; break;
            case VT_UI2: *out = (double)v.uiVal; ok = true; break;
            case VT_I2:  *out = (double)v.iVal;  ok = true; break;
            case VT_UI8: *out = (double)v.ullVal; ok = true; break;
            case VT_I8:  *out = (double)v.llVal;  ok = true; break;
            case VT_R8:  *out = v.dblVal; ok = true; break;
            case VT_R4:  *out = (double)v.fltVal; ok = true; break;
            case VT_BOOL: *out = V_BOOL(&v) ? 1 : 0; ok = true; break;
            case VT_UI1: *out = (double)v.bVal; ok = true; break;
            case VT_I1:  *out = (double)v.cVal; ok = true; break;
            case VT_BSTR: *out = wcstod(v.bstrVal, nullptr); ok = true; break;
            }
        }
        VariantClear(&v);
        obj->Release();
    }
    en->Release();
    return ok;
}

// 每秒：电池动态数据
static void ReadBatteryDynamic() {
    double d = 0;
    const wchar_t* wql = L"SELECT * FROM BatteryStatus";
    g.hasBattery = false;
    if (WmiQueryValue(wql, L"Voltage", &d) && d > 0) { g.voltageV = d / 1000.0; g.hasBattery = true; }
    if (WmiQueryValue(wql, L"PowerOnline", &d)) g.online = d != 0;

    // ChargeRate / DischargeRate：WMI 冷启动时首查可能返回 0（实测 0.3s 后才有值）。
    // 若读到 0 且处于插电态，重试几次，避免把真实充电误判为"无流动"。
    double chg = 0, dis = 0;
    for (int tries = 0; tries < 3; tries++) {
        WmiQueryValue(wql, L"ChargeRate", &chg);
        WmiQueryValue(wql, L"DischargeRate", &dis);
        if (chg > 0 || dis > 0) break;
        if (tries < 2) Sleep(150);
    }
    if (!(chg > 0) && g.online && g.hasBattery) {
        // 插电但两路都为 0：可能是满充/涓流，保持 0 但不清零上次的有效充电值
        // （避免界面在 30W↔0W 之间闪跳）
        if (g.chargeW > 0.05) chg = g.chargeW;
    }
    g.chargeW = chg > 0 ? chg / 1000.0 : 0;
    g.dischargeW = dis > 0 ? dis / 1000.0 : 0;
    g.powerW = g.chargeW > 0 ? g.chargeW : (g.dischargeW > 0 ? -g.dischargeW : 0);
    g.currentA = (g.voltageV > 0.5) ? ((g.chargeW > 0 ? g.chargeW : -g.dischargeW) / g.voltageV) : 0;
    // 电量 = 剩余 / 满充
    double rem = 0, full = 0;
    if (WmiQueryValue(wql, L"RemainingCapacity", &rem) && rem > 0) {
        if (WmiQueryValue(L"SELECT * FROM BatteryFullChargedCapacity", L"FullChargedCapacity", &full) && full > 0) {
            g.percent = rem / full * 100.0;
            g.fullWh = full / 1000.0;
        }
    }
}

// 每 30 秒：设计容量 / 循环次数 / 健康度
static void ReadBatteryStatic() {
    double d = 0;
    if (WmiQueryValue(L"SELECT * FROM BatteryStaticData", L"DesignedCapacity", &d) && d > 0)
        g.designWh = d / 1000.0;
    if (WmiQueryValue(L"SELECT * FROM BatteryCycleCount", L"CycleCount", &d) && d >= 0)
        g.cycles = (long)d;
    if (g.designWh > 0 && g.fullWh > 0) g.health = g.fullWh / g.designWh * 100.0;
}

// CPU 占用：GetSystemTimes 差分
static ULONGLONG s_prevIdle = 0, s_prevKern = 0, s_prevUser = 0;
static bool s_cpuInit = false;
static void ReadCpuPercent() {
    FILETIME ftIdle, ftKern, ftUser;
    if (!GetSystemTimes(&ftIdle, &ftKern, &ftUser)) return;
    auto toULL = [](const FILETIME& ft) {
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; return u.QuadPart; };
    ULONGLONG idle = toULL(ftIdle), kern = toULL(ftKern), user = toULL(ftUser);
    if (s_cpuInit) {
        ULONGLONG dIdle = idle - s_prevIdle, dKern = kern - s_prevKern, dUser = user - s_prevUser;
        ULONGLONG total = dKern + dUser;
        g.cpuPct = total > 0 ? (1.0 - (double)dIdle / (double)total) * 100.0 : 0;
        if (g.cpuPct < 0) g.cpuPct = 0;
    }
    s_prevIdle = idle; s_prevKern = kern; s_prevUser = user; s_cpuInit = true;
}

// SoC 温度：PDH 性能计数器「Thermal Zone Information \ High Precision Temperature」
// （与系统自带监视一致；WMI 的 MSAcpi 类在多数笔记本上不暴露）。原始值单位 0.1 K。
static void ReadTemp() {
    static PDH_HQUERY q = nullptr;
    static PDH_HCOUNTER ctr = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(q, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &ctr) != ERROR_SUCCESS) {
                PdhCloseQuery(q); q = nullptr;
            }
        }
    }
    if (!q || !ctr) return;
    if (PdhCollectQueryData(q) != ERROR_SUCCESS) return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(ctr, PDH_FMT_DOUBLE, &size, &count, nullptr) != PDH_MORE_DATA) return;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)malloc(size);
    if (!items) return;
    if (PdhGetFormattedCounterArrayW(ctr, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
        double maxC = -999;
        for (DWORD i = 0; i < count; i++) {
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) {
                double c = items[i].FmtValue.doubleValue / 10.0 - 273.15;
                if (c > maxC && c < 150) maxC = c;
            }
        }
        if (maxC > -999) g.socTempC = maxC;
    }
    free(items);
}

// GPU 占用：cimv2 GPUEngine 计数器。
//   取 3D / Compute 引擎各实例占用之和，再按"引擎最大占用"归一。
//   注意：同一 GPU 的多个 engine 实例（各进程各队列）之和会远超 100%，
//   旧实现直接求和后截断到 100 会失真。改为取每个 GPU 内的最大实例值再累加。
static void ReadGpuPercent() {
    if (!g_wmi2) return;
    IEnumWbemClassObject* en = nullptr;
    HRESULT hr = g_wmi2->ExecQuery(BStr(L"WQL"),
        BStr(L"SELECT Name, UtilizationPercentage FROM Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    if (FAILED(hr)) return;

    // 按 GPU 实例（Name 里 "luid_0x..._0x..." 前缀）分组，组内取最大 3D/Compute 占用
    struct Grp { wchar_t key[64]; double mx; };
    Grp grps[8]; int ng = 0;
    IWbemClassObject* obj = nullptr; ULONG ret = 0;
    while (en->Next(WBEM_INFINITE, 1, &obj, &ret) == S_OK) {
        VARIANT vn; VariantInit(&vn);
        if (SUCCEEDED(obj->Get(L"Name", 0, &vn, nullptr, nullptr)) &&
            vn.vt == VT_BSTR && vn.bstrVal) {
            const wchar_t* nm = vn.bstrVal;
            bool wanted = wcsstr(nm, L"engtype_3D") || wcsstr(nm, L"engtype_Compute");
            if (wanted) {
                double uv = 0; bool okv = false;
                VARIANT vu; VariantInit(&vu);
                if (SUCCEEDED(obj->Get(L"UtilizationPercentage", 0, &vu, nullptr, nullptr))) {
                    if (vu.vt == VT_UI4) { uv = (double)vu.ulVal; okv = true; }
                    else if (vu.vt == VT_I4) { uv = (double)vu.lVal; okv = true; }
                    else if (vu.vt == VT_UI8) { uv = (double)vu.ullVal; okv = true; }
                }
                VariantClear(&vu);
                if (okv && uv > 0) {
                    // 取前 48 字符作为 GPU 分组键
                    wchar_t key[64] = L""; int k = 0;
                    for (int i = 0; i < 48 && nm[i] && k < 62; i++) key[k++] = nm[i];
                    key[k] = 0;
                    int idx = -1;
                    for (int i = 0; i < ng; i++) if (wcscmp(grps[i].key, key) == 0) { idx = i; break; }
                    if (idx < 0 && ng < 8) { idx = ng++; wcsncpy(grps[idx].key, key, 63); grps[idx].mx = 0; }
                    if (idx >= 0 && uv > grps[idx].mx) grps[idx].mx = uv;
                }
            }
        }
        VariantClear(&vn);
        obj->Release();
    }
    en->Release();

    double best = 0;
    for (int i = 0; i < ng; i++) if (grps[i].mx > best) best = grps[i].mx;
    if (best > 100) best = 100;
    g.gpuPct = best;
}

// ---------------------------------------------------------------------------
// 总功耗估算
//   SoC 功耗模型（按 Ryzen 7 8745H + Radeon 780M 实测标定）：
//     · 空载 SoC 封装功耗 ≈ 17.7 W（APU 底噪高）
//     · 满载 SoC 封装功耗 ≈ 30~36 W
//     · 负载项非线性：低负载陡升、高负载趋缓 → 用 sqrt 曲线贴合
//   整机功耗 = SoC + 基础功耗(主板/内存/SSD/屏/网卡/供电损耗 ≈ 6 W)
// ---------------------------------------------------------------------------
static const double kSocIdleW   = 17.7;   // 空载 SoC 封装功耗（LHM 实测）
static const double kBaseOverW  = 6.0;    // 非 SoC 基础功耗
static const double kChgEff     = 0.90;   // 电池充电效率（充入/输入）

static double EstimateSocW() {
    double cpu = g.cpuPct > 0 ? g.cpuPct : 0;
    double gpu = g.gpuPct > 0 ? g.gpuPct : 0;
    // GPU 项：任一非零才计入（两者同属 SoC 封装，不重复叠加）
    double load = cpu + 0.5 * gpu;                  // GPU 权重折半（同封装内共享）
    if (load < 0) load = 0;
    // 非线性：sqrt 曲线。CPU=100% 时约 +13.5W，逼近满载实测 30~31W
    double soc = kSocIdleW + 1.35 * sqrt(load);
    if (soc > 34.0) soc = 34.0;                     // 本机 SoC 封装上限（PPT）
    return soc;
}

static double EstimateSystemW() {
    return EstimateSocW() + kBaseOverW;             // 整机 = SoC + 基础功耗
}

// 返回总功耗（W）与是否估算值：放电=实测；充电=充电/效率 + 系统；无流动=系统估算
static double GetTotalWatts(bool* estimated, double* systemW) {
    if (g.dischargeW > 0.05) {                      // 放电：整机实测
        if (estimated) *estimated = false;
        if (systemW) *systemW = g.dischargeW;
        return g.dischargeW;
    }
    double sys = EstimateSystemW();
    if (systemW) *systemW = sys;
    if (estimated) *estimated = true;
    if (g.chargeW > 0.05) {
        // 充电：适配器输入 ≈ 系统 + 充入功率/效率（充电有 ~10% 变热）
        return sys + g.chargeW / kChgEff;
    }
    return sys;                                      // 无流动：粗估
}

// ---------------------------------------------------------------------------
// 电源模式（PowrProf，动态加载，免链接库）—— 仅完整版
// 精简版（POWERMON_LITE）整段不编译：不加载 DLL、不读注册表、不写 ini。
// ---------------------------------------------------------------------------
#ifndef POWERMON_LITE
typedef DWORD(WINAPI* PFN_PowerEnumerate)(HKEY, const GUID*, const GUID*, DWORD, DWORD, UCHAR*, DWORD*);
typedef DWORD(WINAPI* PFN_PowerReadFriendlyName)(HANDLE, const GUID*, HANDLE, UCHAR*, UCHAR*, DWORD*);
typedef DWORD(WINAPI* PFN_PowerGetActiveScheme)(HANDLE, GUID**);
typedef DWORD(WINAPI* PFN_PowerSetActiveScheme)(HANDLE, const GUID*);

#define ACCESS_SCHEME_FLAG 16   // POWER_DATA_ACCESSOR::ACCESS_SCHEME

static PFN_PowerEnumerate        s_Enum;
static PFN_PowerReadFriendlyName s_ReadName;
static PFN_PowerGetActiveScheme  s_GetActive;
static PFN_PowerSetActiveScheme  s_SetActive;

static bool PowrInit() {
    // 注意：程序未静态链接 powrprof，必须先 LoadLibrary 再取地址
    HMODULE m = LoadLibraryW(L"powrprof.dll");
    if (!m) return false;
    s_Enum      = (PFN_PowerEnumerate)GetProcAddress(m, "PowerEnumerate");
    s_ReadName  = (PFN_PowerReadFriendlyName)GetProcAddress(m, "PowerReadFriendlyName");
    s_GetActive = (PFN_PowerGetActiveScheme)GetProcAddress(m, "PowerGetActiveScheme");
    s_SetActive = (PFN_PowerSetActiveScheme)GetProcAddress(m, "PowerSetActiveScheme");
    return s_Enum && s_ReadName && s_GetActive && s_SetActive;
}

static bool GuidEq(const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }
static bool IsBalancedName(const wchar_t* n) { return wcsstr(n, L"平衡") || wcsstr(n, L"Balanced"); }
static bool IsUltimateName(const wchar_t* n) { return wcsstr(n, L"卓越") || wcsstr(n, L"Ultimate"); }

// ---- 方案 GUID 持久化 ----
// Windows 会把「卓越性能」这类方案在未激活时隐藏（枚举不到），
// 所以首次见到就记到 %APPDATA%\PowerMon\PowerMon.ini，之后即使隐藏也能直接切。
static void GetIniPath(wchar_t* p, int n) {
    wchar_t dir[MAX_PATH] = L"";
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir) == S_OK) {
        wcscat(dir, L"\\PowerMon");
        CreateDirectoryW(dir, nullptr);
        swprintf(p, n, L"%ls\\PowerMon.ini", dir);
    } else {
        GetModuleFileNameW(nullptr, p, n);
        wchar_t* s = wcsrchr(p, L'\\');
        if (s) wcsncpy(s + 1, L"PowerMon.ini", 20);
    }
}
static void SaveSchemeGuid(const wchar_t* key, const GUID& g) {
    wchar_t path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    wchar_t str[64] = L"";
    if (StringFromGUID2(g, str, 64) > 0)
        WritePrivateProfileStringW(L"schemes", key, str, path);
}
static bool LoadSchemeGuid(const wchar_t* key, GUID* out) {
    wchar_t path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    wchar_t str[64] = L"";
    GetPrivateProfileStringW(L"schemes", key, L"", str, 64, path);
    if (!str[0]) return false;
    return SUCCEEDED(CLSIDFromString(str, out));
}

// 标准「卓越性能」模板 GUID（万一本机从未激活过、ini 也未记过时的兜底）
static const GUID kUltimateTemplate = { 0xe9a42b02,0xd5df,0x448d,{0xaa,0x00,0x03,0xf1,0x47,0x49,0xeb,0x61} };

// 解析注册表键名形式的 GUID（不带大括号，如 20cd3150-d30f-47c1-b2d2-ba7226ec946d）
static bool ParseGuidStr(const wchar_t* s, GUID* g) {
    unsigned int d1 = 0, d2 = 0, d3 = 0, b[8] = { 0 };
    int n = swscanf(s, L"%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
    if (n != 11) return false;
    g->Data1 = d1; g->Data2 = (unsigned short)d2; g->Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; i++) g->Data4[i] = (unsigned char)b[i];
    return true;
}

// 从注册表发现「卓越性能」方案（含隐藏方案；被系统隐藏时 API 枚举不到）
// HKLM\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes\<GUID> -> FriendlyName
static bool FindUltimateFromRegistry(GUID* out) {
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes",
            0, KEY_READ, &root) != ERROR_SUCCESS) return false;

    GUID iniUlt{}; bool hasIni = LoadSchemeGuid(L"ultimate", &iniUlt);
    GUID active{}; bool hasActive = false;
    GUID* actPtr = nullptr;
    if (s_GetActive && s_GetActive(nullptr, &actPtr) == ERROR_SUCCESS && actPtr) {
        active = *actPtr; hasActive = true; LocalFree(actPtr);
    }
    GUID cand{}; bool hasCand = false; int candRank = -1;   // 2=当前激活 1=标准模板 0=其他

    wchar_t sub[128]; DWORD idx = 0;
    while (true) {
        DWORD subLen = 128;
        if (RegEnumKeyExW(root, idx++, sub, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        GUID g;
        if (!ParseGuidStr(sub, &g)) continue;                // 只认 GUID 形式的子键

        wchar_t fn[512] = L"";
        DWORD fnLen = sizeof(fn);
        if (RegGetValueW(root, sub, L"FriendlyName",
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, fn, &fnLen) != ERROR_SUCCESS) continue;

        // FriendlyName 可能是 "@dll,-id,fallback" 间接引用
        wchar_t name[256] = L"";
        if (fn[0] == L'@') {
            if (SHLoadIndirectString(fn, name, 256, nullptr) != S_OK) {
                const wchar_t* comma = wcsrchr(fn, L',');
                if (comma) wcsncpy(name, comma + 1, 255);
            }
        } else {
            wcsncpy(name, fn, 255);
        }
        if (!IsUltimateName(name)) continue;
        if (wcsstr(name, L"移动中心") || wcsstr(name, L"obility")) continue;   // 排除移动中心方案

        if (hasIni && GuidEq(g, iniUlt)) { *out = g; RegCloseKey(root); return true; }  // 记忆优先
        int rank = 0;
        if (hasActive && GuidEq(g, active)) rank = 2;
        else if (GuidEq(g, kUltimateTemplate)) rank = 1;
        if (!hasCand || rank > candRank) { cand = g; hasCand = true; candRank = rank; }
    }
    RegCloseKey(root);
    if (hasCand) { *out = cand; return true; }
    return false;
}

static void ReadSchemes() {
    if (!s_Enum) return;
    bool sawBal = false, sawUlt = false;
    GUID bal{}, ult{};
    for (DWORD i = 0; ; i++) {
        GUID guid; DWORD sz = sizeof(guid);
        DWORD r = s_Enum(nullptr, nullptr, nullptr, ACCESS_SCHEME_FLAG, i, (UCHAR*)&guid, &sz);
        if (r != ERROR_SUCCESS) break;      // ERROR_NO_MORE_ITEMS 结束
        wchar_t name[128] = L"";
        DWORD nb = sizeof(name);
        if (s_ReadName(nullptr, &guid, nullptr, nullptr, (UCHAR*)name, &nb) == ERROR_SUCCESS) {
            if (IsBalancedName(name)) { bal = guid; sawBal = true; }
            if (IsUltimateName(name)) { ult = guid; sawUlt = true; }
        }
    }
    static GUID lastBal{}, lastUlt{};
    static bool savedBal = false, savedUlt = false;
    // 平衡
    if (sawBal) {
        g.schemeBalanced = bal; g.hasBalanced = true;
        if (!savedBal || !GuidEq(lastBal, bal)) { SaveSchemeGuid(L"balanced", bal); lastBal = bal; savedBal = true; }
    } else if (LoadSchemeGuid(L"balanced", &g.schemeBalanced)) {
        g.hasBalanced = true;
    } else g.hasBalanced = false;
    // 卓越性能
    if (sawUlt) {
        g.schemeUltimate = ult; g.hasUltimate = true;
        if (!savedUlt || !GuidEq(lastUlt, ult)) { SaveSchemeGuid(L"ultimate", ult); lastUlt = ult; savedUlt = true; }
    } else if (FindUltimateFromRegistry(&g.schemeUltimate)) {
        g.hasUltimate = true;
        if (!savedUlt || !GuidEq(lastUlt, g.schemeUltimate)) { SaveSchemeGuid(L"ultimate", g.schemeUltimate); lastUlt = g.schemeUltimate; savedUlt = true; }
    } else {
        g.hasUltimate = false;      // 系统确实没有该方案 → 按钮置灰
    }
    // 当前方案
    GUID* act = nullptr;
    if (s_GetActive(nullptr, &act) == ERROR_SUCCESS && act) {
        g.schemeActive = *act;
        wchar_t name[128] = L"";
        DWORD nb = sizeof(name);
        if (s_ReadName(nullptr, act, nullptr, nullptr, (UCHAR*)name, &nb) == ERROR_SUCCESS)
            wcsncpy(g.schemeActiveName, name, 127);
        LocalFree(act);
    }
}
#endif // POWERMON_LITE

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------
static HWND g_hwnd = nullptr;
static HFONT g_fntTitle, g_fntBig, g_fntSub, g_fntCap, g_fntVal, g_fntBtn;
static HBRUSH g_brBg, g_brBar;
#ifndef POWERMON_LITE
static wchar_t g_schemeErr[64] = L"";
static DWORD g_schemeErrTick = 0;
#define IDC_BTN_BAL  2001
#define IDC_BTN_ULT  2002
#endif
#define IDC_TIMER    1

static int UiDpi() { return (int)GetDpiForSystem(); }
static int S(int v) { return MulDiv(v, UiDpi(), 96); }

static HFONT MkFont(int h, int weight) {
    return CreateFontW(-S(h), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

static COLORREF ColCharge() { return RGB(22, 163, 74); }    // 绿
static COLORREF ColDis()    { return RGB(234, 88, 12); }    // 橙
static COLORREF ColIdle()   { return RGB(100, 105, 112); }  // 灰
static COLORREF ColText()   { return RGB(24, 24, 27); }
static COLORREF ColDim()    { return RGB(139, 143, 150); }

static void DrawTxt(HDC dc, const wchar_t* s, RECT& r, HFONT f, COLORREF c) {
    HFONT of = (HFONT)SelectObject(dc, f);
    SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE);
    SelectObject(dc, of);
}

// 圆角矩形（填充 + 1px 描边）
static void FillRoundRect(HDC dc, RECT r, int rad, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void DrawTxtFlags(HDC dc, const wchar_t* s, RECT& r, HFONT f, COLORREF c, UINT flags) {
    HFONT of = (HFONT)SelectObject(dc, f);
    SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, flags);
    SelectObject(dc, of);
}

static void GetPowerDisplay(wchar_t* big, wchar_t* sub, COLORREF& col) {
    if (!g.hasBattery) { wcscpy(big, L"— W"); wcscpy(sub, L"未检测到电池"); col = ColIdle(); return; }
    double w = g.powerW;
    if (w > 0.01)       { swprintf(big, 64, L"%.1f W", w);  col = ColCharge(); }
    else if (w < -0.01) { swprintf(big, 64, L"%.1f W", -w); col = ColDis(); }
    else {
        // 无充放电流动 → 显示整机总功耗（粗估，对齐旧版语义）
        double tot = GetTotalWatts(nullptr, nullptr);
        swprintf(big, 64, L"%.1f W", tot);
        col = ColIdle();
    }
    wchar_t info[128] = L"";
    if (g.voltageV > 0.01)
        swprintf(info, 128, L"电压 %.2f V · 电流 %.2f A", g.voltageV, fabs(g.currentA));
    const wchar_t* st;
    if (w > 0.01)       st = L"充电中";
    else if (w < -0.01) st = L"放电中";
    else                st = g.online ? L"已接通电源 · 系统功耗（粗估）" : L"电池供电 · 系统功耗（粗估）";
    // 注意：msvcrt 的 swprintf 中 %s 表示窄字符串，宽字符串必须用 %ls
    swprintf(sub, 192, L"%ls · %ls", st, info);
}

static void OnPaint(HDC dc, RECT& rc) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HBITMAP ob = (HBITMAP)SelectObject(mem, bmp);
    FillRect(mem, &rc, g_brBg);

    wchar_t buf[256];
    int x = S(24), y = S(14);

    // 标题
    RECT r = { x, y, rc.right - x, y + S(22) };
    DrawTxt(mem, L"充电功率监控", r, g_fntTitle, ColDim());
    y += S(26);

    // 大功率数字
    wchar_t big[64], sub[192]; COLORREF pc;
    GetPowerDisplay(big, sub, pc);
    r = { x, y, rc.right - x, y + S(68) };
    DrawTxt(mem, big, r, g_fntBig, pc);
    y += S(70);
    r = { x, y, rc.right - x, y + S(22) };
    DrawTxt(mem, sub, r, g_fntSub, ColDim());
    y += S(32);

    // ---- 白色卡片 ----
    RECT card = { x, y, rc.right - x, y + S(266) };
    FillRoundRect(mem, card, S(14), RGB(255, 255, 255), RGB(233, 234, 236));
    int cx = card.left + S(18);
    int cw = (card.right - card.left) - S(36);
    int cy = card.top + S(16);

    // 电量：标签 + 数值 + 圆角条
    double pct = g.percent >= 0 ? g.percent : 0;
    r = { cx, cy + S(2), cx + S(100), cy + S(21) };
    DrawTxt(mem, L"电量", r, g_fntCap, ColDim());
    swprintf(buf, 256, L"%.0f%%", pct);
    r = { cx + cw - S(120), cy, cx + cw, cy + S(22) };
    DrawTxtFlags(mem, buf, r, g_fntVal, pct > 20 ? ColText() : RGB(220, 60, 60),
        DT_RIGHT | DT_TOP | DT_SINGLELINE);
    int barY = cy + S(28), barH = S(12);
    RECT barBg = { cx, barY, cx + cw, barY + barH };
    FillRoundRect(mem, barBg, barH / 2, RGB(229, 231, 235), RGB(229, 231, 235));
    int fillW = (int)(cw * pct / 100.0 + 0.5);
    if (fillW >= barH) {
        RECT barFg = { cx, barY, cx + fillW, barY + barH };
        COLORREF fc = pct > 20 ? ColCharge() : RGB(220, 60, 60);
        FillRoundRect(mem, barFg, barH / 2, fc, fc);
    }

    // 指标网格 2 列 × 4 行
    int gy = card.top + S(60);
    int rowH = S(48);
    int colW = (cw - S(12)) / 2;
    auto item = [&](int col, int row, const wchar_t* cap, const wchar_t* val) {
        int ix = cx + col * (colW + S(12));
        int iy = gy + row * rowH;
        RECT r1 = { ix, iy, ix + colW, iy + S(16) };
        DrawTxt(mem, cap, r1, g_fntCap, ColDim());
        RECT r2 = { ix, iy + S(18), ix + colW, iy + S(42) };
        DrawTxt(mem, val, r2, g_fntVal, ColText());
    };
    if (g.health >= 0) swprintf(buf, 256, L"%.1f %%", g.health); else wcscpy(buf, L"—");
    item(0, 0, L"电池健康", buf);
    if (g.cycles >= 0) swprintf(buf, 256, L"%ld 次", g.cycles); else wcscpy(buf, L"—");
    item(1, 0, L"循环次数", buf);
    if (g.fullWh > 0 && g.designWh > 0) swprintf(buf, 256, L"%.1f / %.1f Wh", g.fullWh, g.designWh);
    else if (g.fullWh > 0)              swprintf(buf, 256, L"%.1f Wh", g.fullWh);
    else wcscpy(buf, L"—");
    item(0, 1, L"满充 / 设计容量", buf);
    if (g.cpuPct >= 0) swprintf(buf, 256, L"%.1f %%", g.cpuPct); else wcscpy(buf, L"—");
    item(1, 1, L"CPU 占用", buf);
    if (g.socTempC > -900) swprintf(buf, 256, L"%.1f °C", g.socTempC); else wcscpy(buf, L"—");
    item(0, 2, L"SoC 温度", buf);
    if (g.gpuPct >= 0) swprintf(buf, 256, L"%.1f %%", g.gpuPct); else wcscpy(buf, L"—");
    item(1, 2, L"GPU 占用", buf);
    bool totEst = false; double totSys = 0;
    double totW = GetTotalWatts(&totEst, &totSys);
    if (totEst) swprintf(buf, 256, L"≈ %.1f W", totW); else swprintf(buf, 256, L"%.1f W", totW);
    item(0, 3, L"系统总功耗", buf);
#ifdef POWERMON_LITE
    // 精简版没有切换能力，这一格显示供电状态，避免空档
    item(1, 3, L"供电状态", g.online ? L"已接通电源" : L"电池供电");
#else
    item(1, 3, L"当前电源模式", g.schemeActiveName);
#endif

#ifndef POWERMON_LITE
    // 底部：左侧标签
    y = card.bottom + S(14);
    r = { x + S(4), y, x + S(200), y + S(20) };
    DrawTxt(mem, L"电源模式", r, g_fntSub, ColDim());
    if (g_schemeErr[0] && GetTickCount() - g_schemeErrTick < 6000) {
        r = { x + S(84), y + S(2), rc.right - x, y + S(22) };
        DrawTxt(mem, g_schemeErr, r, g_fntCap, RGB(220, 60, 60));
    }
#endif

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
}

#ifndef POWERMON_LITE
// 自绘按钮（圆角样式）
static void DrawOwnButton(HDC dc, const DRAWITEMSTRUCT* dis) {
    RECT r = dis->rcItem;
    int id = (int)dis->CtlID;
    bool isActive = (id == IDC_BTN_BAL && g.hasBalanced && GuidEq(g.schemeActive, g.schemeBalanced)) ||
                    (id == IDC_BTN_ULT && g.hasUltimate && GuidEq(g.schemeActive, g.schemeUltimate));
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    wchar_t text[64] = L"";
    GetWindowTextW(dis->hwndItem, text, 64);
    FillRect(dc, &r, g_brBg);   // 先铺底色（圆角外的角落）
    if (isActive) {
        COLORREF c = pressed ? RGB(21, 128, 61) : ColCharge();
        FillRoundRect(dc, r, S(10), c, c);
        DrawTxtFlags(dc, text, r, g_fntBtn, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        COLORREF bg = pressed ? RGB(240, 243, 241) : RGB(255, 255, 255);
        FillRoundRect(dc, r, S(10), bg, RGB(212, 216, 220));
        DrawTxtFlags(dc, text, r, g_fntBtn, RGB(58, 62, 68), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void UpdateButtons();

static void ApplyScheme(const GUID& s) {
    DWORD r = 1;
    if (s_SetActive) r = s_SetActive(nullptr, &s);
    if (r == ERROR_SUCCESS) {
        g_schemeErr[0] = 0;
        Sleep(150);
    } else {
        swprintf(g_schemeErr, 64, L"切换失败（错误码 %lu）", r);
        g_schemeErrTick = GetTickCount();
    }
    ReadSchemes();
    UpdateButtons();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void UpdateButtons() {
    HWND b1 = GetDlgItem(g_hwnd, IDC_BTN_BAL);
    HWND b2 = GetDlgItem(g_hwnd, IDC_BTN_ULT);
    if (b1) InvalidateRect(b1, nullptr, FALSE);   // 状态由自绘体现（激活=绿实心）
    if (b2) InvalidateRect(b2, nullptr, FALSE);
}

static void PlaceButtons() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int bw = S(106), bh = S(38), gap = S(10);
    HWND b1 = GetDlgItem(g_hwnd, IDC_BTN_BAL);
    HWND b2 = GetDlgItem(g_hwnd, IDC_BTN_ULT);
    int y = rc.bottom - bh - S(16);
    if (b2) MoveWindow(b2, rc.right - S(24) - bw, y, bw, bh, TRUE);
    if (b1) MoveWindow(b1, rc.right - S(24) - bw * 2 - gap, y, bw, bh, TRUE);
}
#endif // POWERMON_LITE

static int s_tick = 0;
static bool s_snapMode = false;
static DWORD s_snapStart = 0;
#ifndef POWERMON_LITE
static bool s_clickMode = false;
static int s_clickTick = 0;
#endif

// 点击测试日志（UTF-8 追加到 exe 同目录 click-test.log）
static void ClickLog(const wchar_t* fmt, ...) {
    wchar_t buf[512] = L"";
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    char u8[1536] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* sl = wcsrchr(path, L'\\');
    if (sl) wcsncpy(sl + 1, L"click-test.log", 20);
    FILE* f = _wfopen(path, L"ab");
    if (f) {
        fwrite(u8, 1, strlen(u8), f);
        fputc('\n', f);
        fclose(f);
    }
}

// 保存 24 位 BMP（自检截图用）
static bool SaveBmp(HDC dc, HBITMAP bmp, int w, int h, const wchar_t* path) {
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = h;
    bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
    DWORD stride = ((w * 3 + 3) / 4) * 4;
    DWORD imgSize = stride * h;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + imgSize;
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    BYTE* buf = (BYTE*)malloc(imgSize);
    if (buf) {
        GetDIBits(dc, bmp, 0, h, buf, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
        fwrite(buf, imgSize, 1, f);
        free(buf);
    }
    fclose(f);
    return true;
}

static void DoSnapshot(HWND hw) {
    RECT rc; GetWindowRect(hw, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC dc = GetDC(hw);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP ob = (HBITMAP)SelectObject(mem, bmp);
    PrintWindow(hw, mem, PW_RENDERFULLCONTENT);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* s = wcsrchr(path, L'\\');
    if (s) wcsncpy(s + 1, L"PowerMon-snapshot.bmp", 32);
    SaveBmp(mem, bmp, w, h, path);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hw, dc);
}

static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hw, IDC_TIMER, 1000, nullptr);
#ifndef POWERMON_LITE
        CreateWindowW(L"BUTTON", L"平衡", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 10, 10, hw, (HMENU)(INT_PTR)IDC_BTN_BAL, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"卓越性能", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 10, 10, hw, (HMENU)(INT_PTR)IDC_BTN_ULT, nullptr, nullptr);
        PlaceButtons();
#endif
        return 0;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
#ifndef POWERMON_LITE
        if (dis && dis->CtlType == ODT_BUTTON) DrawOwnButton(dis->hDC, dis);
#else
        (void)dis;
#endif
        return TRUE;
    }
    case WM_SIZE:
#ifndef POWERMON_LITE
        PlaceButtons();
#endif
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        ReadBatteryDynamic();
        ReadCpuPercent();
        ReadGpuPercent();
        ReadTemp();
#ifndef POWERMON_LITE
        ReadSchemes();
#endif
        if ((s_tick++ % 30) == 0) ReadBatteryStatic();
#ifndef POWERMON_LITE
        UpdateButtons();
#endif
        InvalidateRect(hw, nullptr, FALSE);
        if (s_snapMode && GetTickCount() - s_snapStart > 4000) {
            UpdateWindow(hw);
            DoSnapshot(hw);
            DestroyWindow(hw);
            return 0;
        }
#ifndef POWERMON_LITE
        if (s_clickMode) {
            // 用 BM_CLICK 模拟真实按钮点击，覆盖「按钮 → WM_COMMAND → 切换」全链路
            if (s_clickTick == 2) {
                ClickLog(L"step1: click [平衡] (before active=%ls)", g.schemeActiveName);
                SendMessageW(GetDlgItem(hw, IDC_BTN_BAL), BM_CLICK, 0, 0);
                ClickLog(L"  after: active=%ls hasBal=%d hasUlt=%d err=%ls",
                    g.schemeActiveName, (int)g.hasBalanced, (int)g.hasUltimate, g_schemeErr);
            } else if (s_clickTick == 5) {
                ClickLog(L"step2: click [卓越性能] (before active=%ls)", g.schemeActiveName);
                SendMessageW(GetDlgItem(hw, IDC_BTN_ULT), BM_CLICK, 0, 0);
                ClickLog(L"  after: active=%ls hasBal=%d hasUlt=%d err=%ls",
                    g.schemeActiveName, (int)g.hasBalanced, (int)g.hasUltimate, g_schemeErr);
            } else if (s_clickTick == 8) {
                ClickLog(L"done");
                DestroyWindow(hw);
                return 0;
            }
            s_clickTick++;
        }
#endif // POWERMON_LITE
        return 0;
    case WM_COMMAND: {
#ifndef POWERMON_LITE
        int id = LOWORD(wp);
        if (id == IDC_BTN_BAL && g.hasBalanced) ApplyScheme(g.schemeBalanced);
        if (id == IDC_BTN_ULT && g.hasUltimate) ApplyScheme(g.schemeUltimate);
#else
        (void)wp;
#endif
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT rc; GetClientRect(hw, &rc);
        OnPaint(dc, rc);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:
        KillTimer(hw, IDC_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // --console 自检模式：读取一次全部数据输出后退出（便于验证/排障）
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
#ifndef POWERMON_LITE
        if (argc > 1 && argv && wcscmp(argv[1], L"--switch-test") == 0) {
            WmiInit(); PowrInit(); ReadSchemes();
            char u8[256] = { 0 };
            auto show = [&](const char* tag) {
                WideCharToMultiByte(CP_UTF8, 0, g.schemeActiveName, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
                printf("%s: active=%s\n", tag, u8);
            };
            show("start");
            if (g.hasBalanced) {
                DWORD r = s_SetActive(nullptr, &g.schemeBalanced);
                Sleep(400); ReadSchemes();
                printf("switch->balanced ret=%lu  ", r); show("after");
            }
            if (g.hasUltimate) {
                DWORD r = s_SetActive(nullptr, &g.schemeUltimate);
                Sleep(400); ReadSchemes();
                printf("switch->ultimate ret=%lu  ", r); show("after");
            }
            LocalFree(argv);
            return 0;
        }
#endif // POWERMON_LITE
        if (argc > 1 && argv && wcscmp(argv[1], L"--console") == 0) {
            WmiInit();
#ifndef POWERMON_LITE
            PowrInit();
#endif
            ReadBatteryDynamic();
            ReadBatteryStatic();
#ifndef POWERMON_LITE
            ReadSchemes();
#endif
            ReadCpuPercent();
            Sleep(1000);
            ReadCpuPercent();
            ReadGpuPercent();
            ReadTemp();
            printf("battery=%d online=%d powerW=%.2f chargeW=%.2f dischargeW=%.2f\n",
                (int)g.hasBattery, (int)g.online, g.powerW, g.chargeW, g.dischargeW);
            printf("voltage=%.3f V current=%.3f A percent=%.1f%%\n", g.voltageV, fabs(g.currentA), g.percent);
            printf("health=%.1f%% cycles=%ld full=%.2fWh design=%.2fWh\n",
                g.health, g.cycles, g.fullWh, g.designWh);
            printf("cpu=%.1f%% gpu=%.1f%% socTemp=%.1f C\n", g.cpuPct, g.gpuPct, g.socTempC);
            {
                bool est = false; double sys = 0;
                double tot = GetTotalWatts(&est, &sys);
                printf("total=%.1fW estimated=%d system=%.1fW\n", tot, (int)est, sys);
            }
            // 中文经 UTF-8 输出（防 msvcrt %ls 在窄 locale 下丢字）
#ifndef POWERMON_LITE
            char u8[256] = { 0 };
            WideCharToMultiByte(CP_UTF8, 0, g.schemeActiveName, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
            printf("schemeActive=%s balanced[%d] ultimate[%d]\n",
                u8, (int)g.hasBalanced, (int)g.hasUltimate);
            printf("variant=full\n");
#else
            printf("variant=lite (power-scheme switching not included)\n");
#endif
            LocalFree(argv);
            return 0;
        }
        if (argv) LocalFree(argv);
    }

    // --snapshot：显示窗口 4 秒后截图保存 BMP 并退出（自检用）
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc > 1 && argv && wcscmp(argv[1], L"--snapshot") == 0) {
            s_snapMode = true;
            s_snapStart = GetTickCount();
        }
#ifndef POWERMON_LITE
        if (argc > 1 && argv && wcscmp(argv[1], L"--click-test") == 0) {
            s_clickMode = true;
        }
#endif
        if (argv) LocalFree(argv);
    }

    WmiInit();
#ifndef POWERMON_LITE
    PowrInit();
#endif
    ReadBatteryDynamic();
    ReadBatteryStatic();
#ifndef POWERMON_LITE
    ReadSchemes();
#endif

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PowerMonWnd";
    RegisterClassW(&wc);

    g_brBg  = CreateSolidBrush(RGB(241, 242, 244));
    g_brBar = CreateSolidBrush(RGB(229, 231, 235));
    g_fntTitle = MkFont(13, FW_NORMAL);
    g_fntBig   = MkFont(54, FW_SEMIBOLD);
    g_fntSub   = MkFont(13, FW_NORMAL);
    g_fntCap   = MkFont(11, FW_NORMAL);
    g_fntVal   = MkFont(16, FW_SEMIBOLD);
    g_fntBtn   = MkFont(13, FW_NORMAL);

#ifdef POWERMON_LITE
    int w = S(440), h = S(436);      // 精简版无按钮行，窗口矮一截
    const wchar_t* kTitle = L"充电功率监控（Lite）";
#else
    int w = S(440), h = S(500);
    const wchar_t* kTitle = L"充电功率监控";
#endif
    RECT wr = { 0, 0, w, h };
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 3;

    g_hwnd = CreateWindowW(wc.lpszClassName, kTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        sx, sy, ww, wh, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
