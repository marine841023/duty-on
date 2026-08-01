/**
 * Tauri bridge — implements `window.petAPI` (the same surface the Electron
 * preload script exposed via contextBridge) on top of the Tauri 2.0 global
 * API. Loaded before renderer.js so `window.petAPI` is available at init.
 *
 * Command argument names are camelCase (Tauri auto-converts to the snake_case
 * Rust parameter names, e.g. `modelUrl` → `model_url`).
 */
(function () {
  const { invoke } = window.__TAURI__.core;
  const { listen } = window.__TAURI__.event;

  // Cache of active event listeners (Tauri listen is async and returns an
  // unlisten fn; we keep registration fire-and-forget to match the Electron
  // ipcRenderer.on() ergonomics the renderer expects).
  function on(event, cb) {
    listen(event, (e) => cb(e.payload));
  }

  window.petAPI = {
    // ===== Events (renderer → main, main → renderer) =====
    onStateUpdate: (cb) => on('state-update', cb),
    onAlert: (cb) => on('alert', cb),
    onUpdateStatus: (cb) => on('update-status', cb),

    // ===== Hooks =====
    installHooks: () => invoke('install_hooks'),
    isHooksInstalled: () => invoke('is_hooks_installed'),

    // ===== Models =====
    getModels: () => invoke('get_models'),
    switchModel: (modelUrl) => invoke('switch_model', { modelUrl }),

    // ===== Per-state motions =====
    getStateMotions: () => invoke('get_state_motions'),
    setStateMotions: (motions) => invoke('set_state_motions', { motions }),

    // ===== Appearance =====
    getAppearance: () => invoke('get_appearance'),
    setFlipHorizontal: (enabled) => invoke('set_flip_horizontal', { enabled }),

    // ===== Language =====
    getLanguage: () => invoke('get_language'),
    setLanguage: (lang) => invoke('set_language', { lang }),

    // ===== Auto-launch =====
    getAutoLaunch: () => invoke('get_auto_launch'),
    setAutoLaunch: (enabled) => invoke('set_auto_launch', { enabled }),

    // ===== Update =====
    checkForUpdates: () => invoke('check_for_updates'),
    installUpdate: () => invoke('install_update'),

    // ===== Test / window control =====
    testAlert: () => invoke('test_alert'),
    dragWindow: (deltaX, deltaY) => invoke('drag_window', { deltaX, deltaY }),
    setClickThrough: (ignore) => invoke('set_click_through', { ignore }),
    // Click-through (Tauri has no {forward:true} mode, so the Rust polling
    // thread owns set_ignore_cursor_events; the renderer reports the clickable
    // rectangles and a force-clickable flag for drag / menu-open states).
    updateClickRegions: (regions) => invoke('update_click_regions', { regions }),
    setForceClickable: (force) => invoke('set_force_clickable', { force }),
    bringToFront: (projectPath) => invoke('bring_to_front', { projectPath }),
    flashAttention: () => invoke('flash_attention'),
    quit: () => invoke('quit'),
    uninstallApp: () => invoke('uninstall_app'),
  };

  // Visual flash attention signal (the main window is skipTaskbar + always-on-
  // top, so taskbar flash is invisible). Pulse the canvas opacity briefly.
  listen('flash', () => {
    const el = document.getElementById('canvas-wrapper') || document.getElementById('pet-container');
    if (!el) return;
    el.style.transition = 'opacity 120ms ease';
    el.style.opacity = '0.4';
    setTimeout(() => { el.style.opacity = '1'; }, 140);
  });
})();
