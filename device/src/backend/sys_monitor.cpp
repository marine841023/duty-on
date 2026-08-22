// 系统指标采样实现 —— sys_monitor.rs 1:1 移植（Win32 API 直采）。

#ifdef _WIN32

#include "backend/sys_monitor.h"

// Winsock/iphlpapi 头顺序（Windows SDK 的经典坑）：
//   1. winsock2/ws2tcpip 必须先于 windows.h，否则旧版 winsock.h 冲突
//   2. netioapi.h（PMIB_IF_TABLE2/GetIfTable2/FreeMibTable）必须在
//      iphlpapi.h 之后 —— 它的函数声明依赖 iphlpapi.h 定义的
//      IPHLPAPI_DLL_LINKAGE/NETIOAPI_API 链接宏；而 iphlpapi.h 对它的
//      包裹又被 NTDDI_VERSION >= NTDDI_VISTA 条件门控，故显式兜底包含
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <psapi.h>

#include <chrono>
#include <cmath>

#include "backend/backend_config.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "psapi.lib")

namespace dutyon::backend {

namespace {

// ---- NVML 动态加载（无 N 卡 / 无驱动时保持未初始化，GPU 行显示 "—"）----
struct Nvml {
    bool available = false;
    std::string gpu_name;
    void* device = nullptr;  // nvmlDevice_t（device 0）

    // 函数指针表（nvml.dll 成功加载并初始化后有效；nvmlReturn_t = int）
    int (*nvmlInit_v2)() = nullptr;
    int (*nvmlDeviceGetHandleByIndex_v2)(unsigned int index, void** device) = nullptr;
    int (*nvmlDeviceGetName)(void* device, char* name, unsigned int length) = nullptr;
    int (*nvmlDeviceGetUtilizationRates)(void* device, void* utilization) = nullptr;
    int (*nvmlDeviceGetMemoryInfo_v2)(void* device, void* memory) = nullptr;
    int (*nvmlDeviceGetMemoryInfo)(void* device, void* memory) = nullptr;  // v1 回退
};

// nvmlDeviceGetUtilizationRates 的返回结构（NVML 头文件里的 nvmlUtilization_t）
struct NvmlUtilization {
    unsigned int gpu;     // 0-100
    unsigned int mem;     // 0-100
    unsigned int enc;     // 0-100
    unsigned int dec;     // 0-100
};

// nvmlDeviceGetMemoryInfo_v2 的返回结构（nvmlMemory_v2_t）
struct NvmlMemoryV2 {
    unsigned int version;        // NVML_STRUCT_VERSION(Memory, 2)
    unsigned int ecc_mode;       // nvmlEnableState_t 枚举（4 字节）
    unsigned long long total;
    unsigned long long reserved;
    unsigned long long free;
    unsigned long long used;
};

// nvmlDeviceGetMemoryInfo（v1）的返回结构（nvmlMemory_t）
struct NvmlMemoryV1 {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

// NVML 结构版本号构造（同 NVML_STRUCT_VERSION 宏：ver | (sizeof<<16)）
constexpr unsigned int nvmlStructVersion(unsigned int ver, size_t size) {
    return ver | ((unsigned int)size << 16);
}

Nvml loadNvml() {
    Nvml nv;
    HMODULE lib = LoadLibraryW(L"nvml.dll");
    if (!lib) return nv;
    auto init = (int (*)())GetProcAddress(lib, "nvmlInit_v2");
    auto byIndex = (int (*)(unsigned int, void**))GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2");
    auto name = (int (*)(void*, char*, unsigned int))GetProcAddress(lib, "nvmlDeviceGetName");
    auto util = (int (*)(void*, void*))GetProcAddress(lib, "nvmlDeviceGetUtilizationRates");
    auto memInfo = (int (*)(void*, void*))GetProcAddress(lib, "nvmlDeviceGetMemoryInfo_v2");
    auto memInfoV1 = (int (*)(void*, void*))GetProcAddress(lib, "nvmlDeviceGetMemoryInfo");
    if (!init || !byIndex || !name || !util || !memInfo) return nv;
    if (init() != 0) return nv;  // nvmlReturn_t::NVML_SUCCESS == 0
    // 出参接口：device 由 NVML 写入（此前签名漏掉出参导致写入垃圾地址崩溃）
    if (byIndex(0, &nv.device) != 0 || !nv.device) return nv;
    char buf[96] = {};
    if (name(nv.device, buf, sizeof(buf)) == 0) nv.gpu_name = buf;
    nv.nvmlInit_v2 = init;  // 仅标记函数表可用
    nv.nvmlDeviceGetName = name;
    nv.nvmlDeviceGetUtilizationRates = util;
    nv.nvmlDeviceGetMemoryInfo_v2 = memInfo;
    nv.nvmlDeviceGetMemoryInfo = memInfoV1;  // 可为空（老驱动才没有）
    nv.available = true;
    return nv;
}

// FILETIME -> 100ns 单位数
uint64_t filetimeToU64(const FILETIME& ft) {
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

} // namespace

SysMonitor::~SysMonitor() { stop(); }

void SysMonitor::start() {
    bool expected = false;
    if (!run_.compare_exchange_strong(expected, true)) return;  // 已在跑
    thread_ = std::thread(&SysMonitor::runLoop, this);
}

void SysMonitor::stop() {
    run_ = false;
    if (thread_.joinable()) thread_.join();
}

void SysMonitor::setActive(bool active) { active_ = active; }

void SysMonitor::pokeActive() { active_ = true; }

std::optional<dutyon::SysMetrics> SysMonitor::takeMetrics() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!latest_.has_value() || consumer_seq_ == producer_seq_) return std::nullopt;
    consumer_seq_ = producer_seq_;
    return latest_;
}

std::optional<dutyon::SysMetrics> SysMonitor::latestMetrics() {
    std::lock_guard<std::mutex> lk(mtx_);
    return latest_;
}

dutyon::SysMetrics SysMonitor::sampleOnce(double elapsed_sec) {
    dutyon::SysMetrics m;

    // ---- CPU：GetSystemTimes 差分（全局，一次 API 调用）----
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const uint64_t i = filetimeToU64(idle), k = filetimeToU64(kernel), u = filetimeToU64(user);
        if (cpu_primed_) {
            const uint64_t di = i - cpu_idle_, dk = k - cpu_kernel_, du = u - cpu_user_;
            const uint64_t total = dk + du;  // kernel 含 idle，两次差分后不重复计
            m.cpu_usage = total > 0 ? (float)(100.0 * (double)(total - di) / (double)total)
                                    : 0.0f;
        }
        cpu_idle_ = i;
        cpu_kernel_ = k;
        cpu_user_ = u;
        cpu_primed_ = true;
    }

    // ---- 内存 ----
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.mem_total = ms.ullTotalPhys;
        m.mem_used = ms.ullTotalPhys - ms.ullAvailPhys;
    }

    // ---- GPU/显存：NVML device 0 ----
    static const Nvml nvml = loadNvml();  // 进程生命周期内初始化一次
    if (nvml.available) {
        m.has_gpu = true;
        m.gpu_name = nvml.gpu_name;
        NvmlUtilization util{};
        if (nvml.nvmlDeviceGetUtilizationRates(nvml.device, &util) == 0) {
            m.gpu_usage = (float)util.gpu;
        }
        NvmlMemoryV2 mem{};
        mem.version = nvmlStructVersion(2, sizeof(NvmlMemoryV2));
        if (nvml.nvmlDeviceGetMemoryInfo_v2(nvml.device, &mem) == 0) {
            m.vram_total = mem.total;
            m.vram_used = mem.used;
        } else if (nvml.nvmlDeviceGetMemoryInfo) {
            // 驱动不支持 v2 时回退 v1（nvmlMemory_t：total/free/used）
            NvmlMemoryV1 m1{};
            if (nvml.nvmlDeviceGetMemoryInfo(nvml.device, &m1) == 0) {
                m.vram_total = m1.total;
                m.vram_used = m1.used;
            }
        }
    }

    // ---- 网络：GetIfTable2 全接口 InOctets/OutOctets 总和差分 ----
    // 虚拟适配器（VPN/WSL/回环）只有真正承载流量才贡献增量，与任务管理器
    // 的"按适配器汇总"接近。每 tick 重新取表，新拔插的网卡自然纳入。
    {
        PMIB_IF_TABLE2 table = nullptr;
        if (GetIfTable2(&table) == NO_ERROR) {
            uint64_t rx = 0, tx = 0;
            for (ULONG i = 0; i < table->NumEntries; i++) {
                const MIB_IF_ROW2& row = table->Table[i];
                rx += row.InOctets;
                tx += row.OutOctets;
            }
            FreeMibTable(table);
            if (net_primed_ && rx >= net_rx_total_ && tx >= net_tx_total_) {
                m.net_rx_rate = (unsigned long long)std::llround(
                    (double)(rx - net_rx_total_) / elapsed_sec);
                m.net_tx_rate = (unsigned long long)std::llround(
                    (double)(tx - net_tx_total_) / elapsed_sec);
            }
            // 计数器回绕/接口移除导致总和变小：本 tick 速率记 0，重置基线
            net_rx_total_ = rx;
            net_tx_total_ = tx;
            net_primed_ = true;
        }
    }

    // ---- 自身 CPU/内存 ----
    {
        const HANDLE self = GetCurrentProcess();
        FILETIME ftCreate, ftExit, ftKernel, ftUser, ftNow;
        if (GetProcessTimes(self, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
            GetSystemTimeAsFileTime(&ftNow);  // 返回 void，不能参与 &&
            const uint64_t proc = filetimeToU64(ftKernel) + filetimeToU64(ftUser);
            const uint64_t wall = filetimeToU64(ftNow);
            if (self_primed_ && wall > self_wall_) {
                const double dproc = (double)(proc - self_proc_time_) / 1e7;  // 100ns -> s
                const double dwall = (double)(wall - self_wall_) / 1e7;
                m.self_cpu = (float)(100.0 * dproc / dwall);  // 多核累计（同 sysinfo）
            }
            self_proc_time_ = proc;
            self_wall_ = wall;
            self_primed_ = true;
        }
        // 私有内存（任务管理器"提交大小"口径，不含与系统 DLL 共享的页面；
        // 工作集在此类 OpenGL/ImGui 进程上会虚高 2-3 倍）
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(self, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            m.self_mem = pmc.PrivateUsage;
        }
    }

    return m;
}

void SysMonitor::runLoop() {
    // NVML 在 loadNvml() 的静态局部里首次调用时初始化（等首个 tick 触发，
    // 不阻塞 start()）
    uint64_t last_tick = 0;
    while (run_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(bc::kMonitorIntervalMs));
        if (!active_) continue;  // 面板关闭：纯 sleep，零系统调用

        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            last_tick == 0
                ? 0.0
                : std::chrono::duration<double>(now.time_since_epoch()).count() - last_tick;
        last_tick = std::chrono::duration<double>(now.time_since_epoch()).count();

        const dutyon::SysMetrics m = sampleOnce(elapsed > 0.05 ? elapsed : 0.05);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            latest_ = m;
            producer_seq_++;
        }
    }
}

} // namespace dutyon::backend

#endif // _WIN32
