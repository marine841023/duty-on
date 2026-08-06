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
    // Fired when the DPI scale changes at runtime (remote-desktop
    // connect/disconnect, monitor/DPI switch); the backend resets the window
    // geometry to its base logical size in the same handler.
    onDisplayChanged: (cb) => on('display-changed', cb),
    // Pull-mode fetch: covers the startup gap where the first state-update
    // fires before this listener is registered (IDE opened before the pet).
    getState: () => invoke('get_state'),
    // Manually reset all sessions to Idle (fallback when the IDE doesn't fire
    // a Stop hook event, e.g., user aborts during the AI's thinking phase).
    resetToIdle: () => invoke('reset_to_idle'),

    // ===== Hooks =====
    installHooks: () => invoke('install_hooks'),
    isHooksInstalled: () => invoke('is_hooks_installed'),

    // ===== Models =====
    getModels: async () => {
      const res = await invoke('get_models');
      // User-uploaded models are served by the local hook server
      // (GET /live2d/*) instead of the Tauri asset protocol: asset-protocol
      // responses carry no CORS headers, so the cubism4/pixi XHR loaders fail
      // preflight with an opaque "Network error" (plain fetch probes return
      // 200 because simple requests skip preflight). The URL path is the
      // model file path relative to ~/.dutyon/live2d/.
      const LIVE2D_URL_BASE = 'http://127.0.0.1:17521/live2d/';
      const LIVE2D_ROOT_MARKER = '/.dutyon/live2d/';
      const toServerUrl = (absPath) => {
        const normalized = String(absPath).replace(/\\/g, '/');
        const idx = normalized.indexOf(LIVE2D_ROOT_MARKER);
        const rel = idx >= 0 ? normalized.slice(idx + LIVE2D_ROOT_MARKER.length) : normalized;
        return LIVE2D_URL_BASE + rel.split('/').map(encodeURIComponent).join('/');
      };
      if (res && Array.isArray(res.models)) {
        res.models = res.models.map((m) =>
          m.userUploaded
            ? { ...m, url: toServerUrl(m.url) }
            : m
        );
      }
      return res;
    },
    switchModel: (modelUrl) => invoke('switch_model', { modelUrl }),
    openLive2DFolder: () => invoke('open_live2d_folder'),
    openSoundsFolder: () => invoke('open_sounds_folder'),

    // ===== External display access =====
    // When true the HTTP server binds 0.0.0.0 so other devices on the LAN can
    // read the read-only /api/* routes (external display). Toggle needs restart.
    getExternalAccess: () => invoke('get_external_access'),
    setExternalAccess: (enabled) => invoke('set_external_access', { enabled }),

    // ===== Per-state motions =====
    getStateMotions: () => invoke('get_state_motions'),
    setStateMotions: (motions) => invoke('set_state_motions', { motions }),

    // ===== Appearance =====
    getAppearance: () => invoke('get_appearance'),
    setFlipHorizontal: (enabled) => invoke('set_flip_horizontal', { enabled }),
    setMiniMode: (enabled) => invoke('set_mini_mode', { enabled }),

    // ===== Edge dock (screen-edge snap, left/right only) =====
    // detectEdgeDock: returns null or "left"/"right" when the window sits
    // within the snap threshold of the monitor's left/right edge.
    detectEdgeDock: () => invoke('detect_edge_dock'),
    // contentHeight (logical px) sizes the compact bar to its badges.
    enterEdgeDock: (edge, contentHeight) => invoke('enter_edge_dock', { edge, contentHeight }),
    exitEdgeDock: () => invoke('exit_edge_dock'),
    // Drag-time ghost preview of the upcoming edge snap: shows a dashed
    // silhouette exactly where the dock bar would land; auto-hides when the
    // window is dragged back inside the snap threshold.
    updateDockPreview: (contentHeight) =>
      invoke('update_dock_preview', { contentHeight }),
    hideDockPreview: () => invoke('hide_dock_preview'),

    // ===== Language =====
    getLanguage: () => invoke('get_language'),
    setLanguage: (lang) => invoke('set_language', { lang }),

    // ===== Auto-launch =====
    getAutoLaunch: () => invoke('get_auto_launch'),
    setAutoLaunch: (enabled) => invoke('set_auto_launch', { enabled }),

    // ===== Test / window control =====
    testAlert: () => invoke('test_alert'),
    // Frontend diagnostic log via invoke (reliable in dev, unlike fetch /log).
    debugLog: (msg) => invoke('debug_log', { msg }),
    dragWindow: (deltaX, deltaY) => invoke('drag_window', { deltaX, deltaY }),
    // Make room for the context menu beside the pet. Two-phase: calculate
    // picks the side/delta WITHOUT moving the window (lets the renderer stage
    // its translateX first); apply does the actual resize/shift. Splitting is
    // required so the transform is staged before the WM_SIZE-triggered
    // composite, otherwise the pet visibly jumps on left-side menus.
    calculateMenuSpace: (width) => invoke('calculate_menu_space', { width }),
    applyMenuSpace: (side, delta) => invoke('apply_menu_space', { side, delta }),
    closeMenuSpace: () => invoke('close_menu_space'),
    // Separate menu window (zero-flicker): show/hide a second Tauri window
    // beside the pet. The pet window never resizes → no layout lag.
    showMenuWindow: (x, y, w, h) => invoke('show_menu_window', { x, y, w, h }),
    hideMenuWindow: () => invoke('hide_menu_window'),
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
