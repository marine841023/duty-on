//! System metrics sampler for the monitor drawer: CPU, RAM, GPU (NVIDIA via
//! NVML), network throughput, and DutyOn's own CPU/RAM footprint. A dedicated
//! OS thread samples every `MONITOR_INTERVAL_MS` and emits `sys-metrics` to
//! the frontend. While the drawer is closed (`MONITOR_ACTIVE == false`) the
//! loop keeps sleeping at zero cost — no sysinfo/NVML calls at all.
//!
//! Cost budget: only precise refreshes are used (global CPU counters, memory,
//! self process, networks) — never `refresh_all()`/`refresh_processes()`,
//! which would walk every process on the system each tick.

use crate::config;
use serde::Serialize;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};
use sysinfo::{Networks, System};
use tauri::Emitter;

/// Master switch: true while the monitor drawer is enabled. When false the
/// sampling loop skips all work (pure sleep).
pub static MONITOR_ACTIVE: AtomicBool = AtomicBool::new(false);

/// Latest sample, shared with the HTTP `/api/metrics` endpoint so native
/// (non-WebView) frontends — the 2.0 C++ desktop pet and the ARM device
/// client — can poll metrics without a Tauri event subscription.
pub static LATEST_METRICS: RwLock<Option<Arc<MetricsSnapshot>>> = RwLock::new(None);

/// One sample. Byte counts are raw bytes; rates are bytes/sec. GPU fields
/// are None when no NVIDIA driver is available — the frontend shows "—".
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MetricsSnapshot {
    pub cpu_usage: f32,
    pub mem_total: u64,
    pub mem_used: u64,
    pub gpu_name: Option<String>,
    pub gpu_usage: Option<f32>,
    pub vram_total: Option<u64>,
    pub vram_used: Option<u64>,
    pub net_rx_rate: u64,
    pub net_tx_rate: u64,
    pub self_cpu: f32,
    pub self_mem: u64,
}

/// Spawn the sampling thread (called once from setup).
pub fn spawn(app: tauri::AppHandle) {
    std::thread::spawn(move || run_loop(app));
}

/// DutyOn's own memory footprint, in the same terms the Task Manager's
/// default "memory" column uses: PRIVATE memory, excluding pages shared with
/// other processes (WebView2 loader, GPU driver and system DLLs). On a
/// Tauri/WebView2 app the full working set (sysinfo's Process::memory()) is
/// easily 2-3x larger, which reads as an inflated number next to Task
/// Manager. Non-Windows platforms keep RSS — that's what Activity Monitor
/// and `top` show. Falls back to RSS/0 if the query fails.
fn self_memory_bytes(p: &sysinfo::Process) -> u64 {
    #[cfg(windows)]
    {
        use windows::Win32::Foundation::CloseHandle;
        use windows::Win32::System::ProcessStatus::{
            GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS_EX,
        };
        use windows::Win32::System::Threading::{
            OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_VM_READ,
        };

        unsafe {
            let Ok(handle) = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                false,
                std::process::id(),
            ) else {
                return p.memory();
            };
            let mut counters = PROCESS_MEMORY_COUNTERS_EX::default();
            let queried = GetProcessMemoryInfo(
                handle,
                &mut counters as *mut PROCESS_MEMORY_COUNTERS_EX as *mut _,
                std::mem::size_of::<PROCESS_MEMORY_COUNTERS_EX>() as u32,
            )
            .is_ok();
            let _ = CloseHandle(handle);
            if !queried {
                return p.memory();
            }
            // PrivateUsage = private committed bytes (Task Manager: 提交大小,
            // close to the private working set shown in the Processes tab).
            counters.PrivateUsage as u64
        }
    }
    #[cfg(not(windows))]
    {
        p.memory()
    }
}

fn run_loop(app: tauri::AppHandle) {
    let mut sys = System::new();
    let self_pid = sysinfo::get_current_pid().ok();
    // NVML initializes once; a missing NVIDIA driver leaves it None forever
    // (the GPU row degrades to "—" instead of erroring on every tick).
    let nvml = nvml_wrapper::Nvml::init().ok();
    let device = nvml.as_ref().and_then(|n| n.device_by_index(0).ok());
    if nvml.is_some() && device.is_none() {
        log::warn!("[monitor] NVML present but device 0 unavailable — GPU metrics disabled");
    }
    let gpu_name: Option<String> = device.as_ref().and_then(|d| d.name().ok());

    // Prime the CPU counters so the first emitted sample already has a usage
    // value (sysinfo needs two refreshes ≥ MINIMUM_CPU_UPDATE_INTERVAL apart
    // to compute a percentage; our 1.5s tick always satisfies that).
    sys.refresh_cpu();
    let mut last_tick = Instant::now();
    // Network counters: sysinfo 0.30 keeps Networks separate from System;
    // received()/transmitted() return the delta since the previous refresh,
    // which is exactly the per-tick bytes we need.
    let mut networks = Networks::new_with_refreshed_list();
    // refresh() misses interfaces added/removed after startup (VPN up/down),
    // so rebuild the list once a minute. refresh_list() resets the delta
    // baseline, which zeroes one tick of rate data — acceptable.
    let mut list_refresh_tick: u32 = 0;

    loop {
        std::thread::sleep(Duration::from_millis(config::MONITOR_INTERVAL_MS));
        if !MONITOR_ACTIVE.load(Ordering::SeqCst) {
            continue;
        }
        let elapsed = last_tick.elapsed().as_secs_f64().max(0.05);
        last_tick = Instant::now();

        sys.refresh_cpu();
        sys.refresh_memory();
        if let Some(pid) = self_pid {
            sys.refresh_process(pid);
        }
        list_refresh_tick = list_refresh_tick.wrapping_add(1);
        if list_refresh_tick % 40 == 0 {
            networks.refresh_list();
        }
        networks.refresh();

        let cpu_usage = sys.global_cpu_info().cpu_usage();
        let (mem_total, mem_used) = (sys.total_memory(), sys.used_memory());

        let (gpu_usage, vram_total, vram_used) = match device.as_ref() {
            Some(d) => {
                let mem = d.memory_info().ok();
                (
                    d.utilization_rates().ok().map(|u| u.gpu as f32),
                    mem.as_ref().map(|m| m.total),
                    mem.map(|m| m.used),
                )
            }
            None => (None, None, None),
        };

        // Sum rx/tx deltas across interfaces. Virtual adapters (VPN, WSL,
        // loopback) contribute only when they actually carry traffic, which
        // closely matches the task manager's per-adapter total.
        let mut rx_total = 0u64;
        let mut tx_total = 0u64;
        for (_iface, data) in &networks {
            rx_total = rx_total.saturating_add(data.received());
            tx_total = tx_total.saturating_add(data.transmitted());
        }
        let net_rx_rate = (rx_total as f64 / elapsed).round() as u64;
        let net_tx_rate = (tx_total as f64 / elapsed).round() as u64;

        let (self_cpu, self_mem) = self_pid
            .and_then(|pid| sys.process(pid))
            .map(|p| (p.cpu_usage(), self_memory_bytes(&p)))
            .unwrap_or((0.0, 0));

        let snapshot = MetricsSnapshot {
            cpu_usage,
            mem_total,
            mem_used,
            gpu_name: gpu_name.clone(),
            gpu_usage,
            vram_total,
            vram_used,
            net_rx_rate,
            net_tx_rate,
            self_cpu,
            self_mem,
        };
        let snapshot = Arc::new(snapshot);
        *LATEST_METRICS.write().unwrap() = Some(snapshot.clone());
        let _ = app.emit("sys-metrics", &*snapshot);
    }
}
