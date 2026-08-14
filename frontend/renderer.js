/**
 * Renderer - Live2D pet rendering, state animations, and UI management.
 *
 * Responsibilities:
 * 1. Initialize PixiJS + Live2D model
 * 2. Manage three pet states: sleeping, working, alert
 * 3. Render status bar with project list
 * 4. Handle drag, context menu, and IPC
 * 5. Fallback canvas animation if Live2D fails
 */

// ===== State =====
let pixiApp = null;
let live2dModel = null;
let currentState = 'sleeping';
let currentSnapshot = { overallState: 'sleeping', sessions: [] };
let effectsTimer = null;
let hooksInstalled = false;    // cached hook install status (for hint refresh)
let oneShotPlaying = false;    // a tap-triggered motion is playing; state motion resumes when it ends
let motionPreview = null;      // [group, idx] looped while hovering the 播放动作 menu
let customOneShotTimer = null; // restore timer for custom-animation one-shots (GIF/MP4 have no motionFinish)
let availableModels = [];      // catalog from main process
let currentModelUrl = null;    // persisted/loaded model URL
let motionPickerMode = 'play'; // 'play' = tap-to-play, 'assign' = pick for a state
let motionPickerTarget = null; // target state when motionPickerMode === 'assign'
let flipHorizontal = false;   // mirror the model horizontally (for left-side placement)
let miniMode = false;         // half-size window mode (调整大小 menu toggle)
let edgeDocked = false;       // window docked as a thin bar at a screen edge
let edgeDockEl = null;        // #edge-dock-bar element (lazily created)
let edgeDockDragEndAt = 0;    // last dock-bar drag end (suppresses stray clicks)
let headEffectEl = null;        // #head-effect container, positioned at head-top anchor
let isDragging = false;         // shared with setupDrag so click-through can stay off while dragging
let currentMotionGroups = [];   // [name, count] pairs scanned from the loaded model's motion defs
let activeCharacter = null;    // { id, name, type: 'live2d'|'animation', animations?: {...} }
let customAnimEl = null;       // #custom-animation container div (video/img inside)
let charactersData = null;     // { builtin: [], custom: [], active: 'id' } from get_characters
let editingCharId = null;      // character ID currently being edited in the edit view

// Model URLs to try (in order)
const MODEL_URLS = [
  'assets/live2d/nito.model3.json',
  'https://cdn.jsdelivr.net/gh/guansss/pixi-live2d-display/test/assets/hiyori/hiyori_pro_t10.model3.json',
];

// ===== Constants =====
const PIXI_WIDTH = 240;
const PIXI_HEIGHT = 260;
// Mini mode canvas: half of the normal size (人物宽高 1/2).
const MINI_PIXI_WIDTH = 120;
const MINI_PIXI_HEIGHT = 130;
const EFFECT_INTERVAL_MS = 2000;
// Motion priorities (match pixi-live2d-display MotionPriority: 1=IDLE, 2=NORMAL, 3=FORCE).
const MOTION_PRIORITY_NORMAL = 2;
const MOTION_PRIORITY_FORCE = 3;
const STATE_COLORS = {
  sleeping: '#7b9eff',
  working: '#ffc832',
  alert: '#ff6666',
};

/**
 * Get the localized state label for the status bar.
 */
function getStateLabel(state) {
  return i18n.t(`state.${state}`);
}

// ===== i18n =====
/**
 * Load the UI language from the main process (persisted choice or OS locale)
 * and set it on the i18n module. Called once during init, before any UI text
 * is rendered.
 */
async function loadLanguage() {
  if (!window.petAPI || !window.petAPI.getLanguage) return;
  try {
    const locale = await window.petAPI.getLanguage();
    const lang = i18n.normalizeLocale(locale);
    i18n.setLanguage(lang);
  } catch (err) {
    console.warn('[i18n] Failed to load language:', err.message);
  }
}

/**
 * Apply the current language to all static DOM text. Elements marked with
 * data-i18n="key" get their textContent set from the translation table.
 * Called on init and after a language switch. Dynamic text (status hints,
 * project list, state label) is refreshed separately by their own updaters.
 */
function applyTranslations() {
  document.querySelectorAll('[data-i18n]').forEach((el) => {
    const key = el.dataset.i18n;
    if (key) el.textContent = i18n.t(key);
  });
  // Refresh dynamic text that depends on the current language.
  updateStateUI(currentState);
  buildMotionMenu(motionPickerMode, motionPickerTarget);
  buildSettingsMenu();
  buildLanguageMenu();
  buildCharEditMenu();
  loadAutoLaunchState();
  loadExternalAccessState();
  updateHookStatusHint(hooksInstalled, currentSnapshot);
  checkHooksStatus();
  // Re-seed the project list so the "waiting" placeholder uses the new language.
  if (currentSnapshot && currentSnapshot.sessions) {
    updateStatusBar(currentSnapshot);
  } else {
    updateStatusBar({ overallState: currentState, sessions: [] });
  }
}

/**
 * Build the language picker submenu from i18n.getLanguages(). The current
 * language is checkmarked. Clicking an item switches the language, persists
 * it, and re-translates the entire UI in place.
 */
function buildLanguageMenu() {
  const container = document.getElementById('language-list');
  if (!container) return;
  container.innerHTML = '';
  for (const lang of i18n.getLanguages()) {
    const item = document.createElement('div');
    item.className = 'menu-item';
    item.textContent = lang.name;
    if (lang.code === i18n.currentLanguage) item.classList.add('active');
    item.addEventListener('click', () => {
      if (lang.code === i18n.currentLanguage) return;
      switchLanguage(lang.code);
    });
    container.appendChild(item);
  }
}

/**
 * Switch the UI language, persist the choice, and re-translate everything.
 * The menu stays open so the user sees the ✓ update.
 */
function switchLanguage(lang) {
  i18n.setLanguage(lang);
  if (window.petAPI && window.petAPI.setLanguage) {
    window.petAPI.setLanguage(lang);
  }
  applyTranslations();
}

// ===== Auto-launch (start on boot) =====
/**
 * Load the current auto-launch state from the main process and update the
 * menu checkmark accordingly.
 */
async function loadAutoLaunchState() {
  const item = document.getElementById('menu-auto-launch');
  if (!item) return;
  try {
    const enabled = window.petAPI && window.petAPI.getAutoLaunch
      ? await window.petAPI.getAutoLaunch()
      : false;
    item.classList.toggle('active', enabled);
  } catch (err) {
    console.warn('[autoLaunch] Failed to read state:', err.message);
  }
}

/**
 * Toggle auto-launch on/off. Persists via main process (registry on Windows).
 */
async function toggleAutoLaunch() {
  const item = document.getElementById('menu-auto-launch');
  if (!item) return;
  const newState = !item.classList.contains('active');
  if (window.petAPI && window.petAPI.setAutoLaunch) {
    window.petAPI.setAutoLaunch(newState);
  }
  item.classList.toggle('active', newState);
}

// ===== External display access =====
/**
 * Load the external_access flag from config and update the menu checkmark.
 * When enabled, the HTTP server binds 0.0.0.0 so other devices on the LAN
 * can read the read-only /api/* routes (external display). Takes effect on
 * the next restart.
 */
async function loadExternalAccessState() {
  const item = document.getElementById('menu-external-access');
  if (!item) return;
  try {
    const enabled = window.petAPI && window.petAPI.getExternalAccess
      ? await window.petAPI.getExternalAccess()
      : false;
    item.classList.toggle('active', enabled);
  } catch (err) {
    console.warn('[externalAccess] Failed to read state:', err.message);
  }
}

/**
 * Toggle external access on/off. Persists via main process; the change needs
 * an app restart to take effect (the live listener's bind address can't
 * change).
 */
async function toggleExternalAccess() {
  const item = document.getElementById('menu-external-access');
  if (!item) return;
  const newState = !item.classList.contains('active');
  if (window.petAPI && window.petAPI.setExternalAccess) {
    window.petAPI.setExternalAccess(newState);
  }
  item.classList.toggle('active', newState);
}

// ===== Initialize =====
async function init() {
  // Menu window mode: skip all Live2D/pet initialization. This window only
  // hosts the context menu, communicating with the main pet window via Tauri
  // events. The pet window never resizes → zero flicker on both sides.
  if (window.__MENU_MODE__) {
    return initMenuMode();
  }
  // Load the UI language (persisted or OS default) before any UI text renders.
  await loadLanguage();
  applyTranslations();

  setupIPC();
  setupContextMenu();
  setupDrag();
  // setupClickThrough() is called from initPixiApp() once pixiApp exists (it
  // registers a PixiJS ticker callback).

  // Fetch available models + persisted choice, per-state motion assignments,
  // and appearance settings in parallel — they have no inter-dependencies.
  await Promise.all([
    loadStateMotions(),
    loadAppearance(),
    loadCharacters(),
  ]);
  // Select the animation backend based on the active character's type so
  // buildSettingsMenu and all motion-related calls use the right backend.
  selectAnimBackend();
  buildSettingsMenu();
  updateFlipMenuCheck();

  // Wait for libraries to load
  const libsReady = await waitForLibs();

  if (libsReady) {
    await initLive2D();
  }

  // After the main model loads, generate thumbnails for all other Live2D
  // models in the background (hidden PIXI app). Cached on disk, so this only
  // runs once per model. Delayed 3s so it doesn't compete with startup.
  if (live2dModel) {
    setTimeout(() => generateMissingThumbnails(), 3000);
  }

  if (!live2dModel && !(activeCharacter && activeCharacter.type === 'animation')) {
    // Fallback to canvas animation — only when there's no Live2D model AND
    // no custom GIF/MP4 character active (otherwise the fallback canvas would
    // stack on top of the custom animation).
    initFallbackCanvas();
    setPetState('sleeping');
  }

  // Ensure the "初始化中..." placeholder is replaced with the real state
  // label. setPetState('sleeping') is a no-op when currentState is already
  // 'sleeping' (its guard clause returns early), so we must call updateStateUI
  // directly to refresh the text + status bar.
  updateStateUI(currentState);

  // Pull the current state once. The scanner's first state-update fires while
  // the webview is still loading, so without this pull an IDE opened before
  // the pet would stay invisible until the next state change.
  try {
    const snap = window.petAPI && window.petAPI.getState ? await window.petAPI.getState() : null;
    if (snap) {
      updateStatusBar(snap);
      updateHookStatusHint(hooksInstalled, snap);
      if (snap.overallState !== currentState) setPetState(snap.overallState);
    }
  } catch (err) {
    console.warn('[init] getState failed:', err && err.message);
  }

  // Start effects loop
  startEffectsLoop();

  // Check if hooks are installed + show diagnostic status
  checkHooksStatus();

  // Health check: detect if backend HTTP server is down (e.g. port 17521
  // occupied). The app window still shows but the pet stays "sleeping"
  // forever — this probes /health and surfaces a localized warning after 3
  // consecutive failures (with retry), auto-clearing on recovery.
  let healthFailCount = 0;
  setTimeout(() => {
    setInterval(async () => {
      // Fetch with a 5s timeout and one immediate retry. Without the timeout
      // a hung connection (rare but possible on Windows loopback) would block
      // the interval indefinitely; the retry absorbs transient blips so they
      // don't inflate the failure counter.
      const probe = async () => {
        const controller = new AbortController();
        const tid = setTimeout(() => controller.abort(), 5000);
        try {
          const resp = await fetch('http://127.0.0.1:17521/health', { signal: controller.signal });
          clearTimeout(tid);
          if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        } finally {
          clearTimeout(tid);
        }
      };
      try {
        try { await probe(); }
        catch (firstErr) { await probe(); /* retry once */ }
        if (healthFailCount > 0) {
          healthFailCount = 0;
          updateStateUI(currentState);
        }
      } catch (e) {
        healthFailCount++;
        if (healthFailCount >= 3) {
          const el = document.getElementById('pet-state-text');
          if (el) el.textContent = i18n.t('status.serverDown');
        }
      }
    }, 30000);
  }, 5000);
}

// ===== Menu window mode =====
// This runs in the separate menu Tauri window (zero-flicker approach). The
// menu window loads the same index.html with window.__MENU_MODE__ = true
// (injected by Rust's initialization_script). It skips all Live2D/pet init
// and only hosts the context menu, communicating with the main pet window
// via Tauri events.

/**
 * Initialize the menu window: load i18n, fetch menu data from Tauri commands,
 * set up event listeners that forward actions to the main window, and listen
 * for live data updates (motion groups, etc.) from the main window.
 */
async function initMenuMode() {
  const { emit } = window.__TAURI__.event;

  // ---- Helpers (must be defined before build function overrides) ----
  const sendAction = (action, params) => {
    emit('menu-action', { action, params: params || {} });
  };
  const closeSelf = () => {
    emit('menu-close', {});
    if (window.petAPI && window.petAPI.hideMenuWindow) window.petAPI.hideMenuWindow();
  };

  // ---- Override build functions BEFORE applyTranslations/data loading ----
  // applyTranslations() and loadCharacters() call buildCharacterGrid /
  // buildLanguageMenu / buildMotionMenu / buildSettingsMenu. The overrides
  // must be in place first so these calls produce items with menu-window
  // event handlers (emit to main) instead of main-window handlers.

  buildLanguageMenu = function () {
    const container = document.getElementById('language-list');
    if (!container) return;
    container.innerHTML = '';
    for (const lang of i18n.getLanguages()) {
      const item = document.createElement('div');
      item.className = 'menu-item';
      item.textContent = lang.name;
      if (lang.code === i18n.currentLanguage) item.classList.add('active');
      item.addEventListener('click', () => {
        if (lang.code === i18n.currentLanguage) return;
        i18n.setLanguage(lang.code);
        if (window.petAPI && window.petAPI.setLanguage) window.petAPI.setLanguage(lang.code);
        applyTranslations();
        sendAction('switchLanguage', { lang: lang.code });
      });
      container.appendChild(item);
    }
  };

  buildCharacterGrid = function () {
    const container = document.getElementById('character-grid');
    if (!container || !charactersData) return;
    container.innerHTML = '';
    const all = [...(charactersData.builtin || []), ...(charactersData.custom || [])];
    for (const char of all) {
      const card = document.createElement('div');
      card.className = 'char-card';
      if (char.id === charactersData.active) card.classList.add('active');
      // Thumbnail
      const thumb = document.createElement('div');
      thumb.className = 'char-thumb';
      if (char.type === 'animation' && char.animations && char.animations.sleeping) {
        const url = animAssetUrl(char.animations.sleeping, char.versions && char.versions.sleeping);
        thumb.style.backgroundImage = `url("${url}")`;
      } else if (char.thumbnail) {
        const url = window.__TAURI__.core.convertFileSrc(char.thumbnail);
        thumb.style.backgroundImage = `url("${url}")`;
      } else {
        thumb.textContent = char.name.charAt(0).toUpperCase();
      }
      card.appendChild(thumb);
      // Name
      const name = document.createElement('div');
      name.className = 'char-name';
      name.textContent = char.name;
      card.appendChild(name);
      // Edit button for custom characters
      if (char.type === 'animation') {
        const editBtn = document.createElement('div');
        editBtn.className = 'char-edit-btn';
        editBtn.textContent = '✎';
        editBtn.addEventListener('click', (e) => {
          e.stopPropagation();
          showEditCharacterView(char.id);
        });
        card.appendChild(editBtn);
      }
      // Click to switch
      card.addEventListener('click', () => {
        if (char.id === charactersData.active) { closeSelf(); return; }
        handleSwitchCharacter(char.id);
      });
      container.appendChild(card);
    }
  };

  buildMotionMenu = function (mode, targetState) {
    const container = document.getElementById('motion-list');
    if (!container) return;
    container.innerHTML = '';
    const label = document.getElementById('motion-view-label');
    if (label) {
      label.textContent = mode === 'assign'
        ? i18n.t('menu.actionSettings') + ' → ' + i18n.t('settings.' + targetState)
        : i18n.t('menu.playMotion');
    }
    const list = animBackend ? animBackend.getMotionList() : [];
    if (!list.length) {
      const empty = document.createElement('div');
      empty.className = 'menu-item';
      empty.style.color = 'rgba(255,255,255,0.35)';
      empty.style.fontStyle = 'italic';
      empty.textContent = i18n.t('menu.noMotions') || '(no motions)';
      container.appendChild(empty);
      return;
    }
    const desiredCurrent = (mode === 'assign' && targetState) ? STATE_MOTIONS[targetState] : null;
    const resolvedCurrent = desiredCurrent
      ? resolveAvailableMotion(desiredCurrent[0], desiredCurrent[1])
      : null;
    for (const [group, count] of list) {
      for (let idx = 0; idx < count; idx++) {
        const item = document.createElement('div');
        item.className = 'menu-item';
        item.textContent = animBackend.getMotionName(group, idx);
        if (resolvedCurrent && resolvedCurrent[0] === group && resolvedCurrent[1] === idx) {
          item.classList.add('active');
        }
        item.addEventListener('mouseenter', () => {
          sendAction('previewMotion', { group, idx });
        });
        item.addEventListener('mouseleave', () => {
          sendAction('stopPreview', {});
        });
        item.addEventListener('click', () => {
          if (mode === 'assign') {
            sendAction('assignMotion', { state: targetState, group, idx });
            showView('menu-settings-view');
          } else {
            sendAction('playMotion', { group, idx });
            closeSelf();
          }
        });
        container.appendChild(item);
      }
    }
  };

  // ---- Load language + apply translations (uses overridden build funcs) ----
  await loadLanguage();
  applyTranslations();

  // ---- Mark body as menu-window + show context menu ----
  document.body.classList.add('menu-window');
  document.getElementById('context-menu').classList.remove('hidden');
  // Fit the window to the main menu content immediately (before async data
  // loads) so the initial open doesn't flash the wrong height.
  fitMenuWindow();

  // ---- Load data from Tauri commands ----
  try {
    await Promise.all([
      loadCharacters(),
      loadStateMotions(),
      loadAppearance(),
    ]);
  } catch (err) {
    if (window.__petSendLog) window.__petSendLog('error', '[menu] initMenuMode data load failed: ' + (err && err.message ? err.message : String(err)));
  }
  // Select the animation backend based on the active character's type so
  // buildMotionMenu / buildSettingsMenu work uniformly in the menu window.
  selectAnimBackend();
  loadAutoLaunchState();
  loadExternalAccessState();

  // ---- Listen for live data from the main window ----
  window.__TAURI__.event.listen('menu-data', (e) => {
    const data = e.payload || {};
    if (data.motions) currentMotionGroups = data.motions;
    if (data.flipHorizontal !== undefined) {
      flipHorizontal = data.flipHorizontal;
      updateFlipMenuCheck();
    }
    if (data.miniMode !== undefined) {
      miniMode = data.miniMode;
      const item = document.getElementById('menu-mini-mode');
      if (item) item.classList.toggle('active', miniMode);
    }
    if (data.hooksInstalled !== undefined) {
      hooksInstalled = data.hooksInstalled;
    }
    if (data.hookStatusHint !== undefined) {
      const el = document.getElementById('hook-status-hint');
      if (el) el.textContent = data.hookStatusHint;
    }
    if (data.currentState) {
      currentState = data.currentState;
    }
    if (data.sessions) {
      currentSnapshot = { overallState: currentState, sessions: data.sessions };
    }
    buildMotionMenu(motionPickerMode, motionPickerTarget);
    buildSettingsMenu();
  });

  // ---- Static event listeners (submenu navigation + actions) ----
  if (window.__petSendLog) window.__petSendLog('info', '[menu] Setting up static event listeners');
  document.getElementById('menu-models-trigger').addEventListener('click', () => {
    showView('menu-model-view');
  });
  document.getElementById('menu-play-motion').addEventListener('click', () => {
    openMotionView('play');
  });
  document.getElementById('menu-settings-trigger').addEventListener('click', () => {
    buildSettingsMenu();
    showView('menu-settings-view');
  });
  document.getElementById('settings-sleeping').addEventListener('click', () => {
    openMotionView('assign', 'sleeping');
  });
  document.getElementById('settings-working').addEventListener('click', () => {
    openMotionView('assign', 'working');
  });
  document.getElementById('settings-alert').addEventListener('click', () => {
    openMotionView('assign', 'alert');
  });
  document.getElementById('menu-language-trigger').addEventListener('click', () => {
    showView('menu-language-view');
  });
  document.getElementById('menu-language-back').addEventListener('click', () => {
    showView('menu-main-view');
  });
  document.getElementById('menu-model-back').addEventListener('click', () => {
    showView('menu-main-view');
  });
  document.getElementById('menu-motion-back').addEventListener('click', () => {
    showView(motionPickerMode === 'assign' ? 'menu-settings-view' : 'menu-main-view');
  });
  document.getElementById('menu-settings-back').addEventListener('click', () => {
    showView('menu-main-view');
  });

  // New character: toggle inline name input below the button
  document.getElementById('menu-new-character').addEventListener('click', (e) => {
    e.stopPropagation();
    handleCreateCharacter();
  });
  // Inline input: confirm / cancel / Enter key
  document.getElementById('char-create-ok').addEventListener('click', (e) => {
    e.stopPropagation();
    confirmCreateCharacter();
  });
  document.getElementById('char-create-cancel').addEventListener('click', (e) => {
    e.stopPropagation();
    document.getElementById('char-create-inline').classList.add('hidden');
    fitMenuWindow();
  });
  document.getElementById('char-create-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); e.stopPropagation(); confirmCreateCharacter(); }
    else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); document.getElementById('char-create-inline').classList.add('hidden'); fitMenuWindow(); }
  });
  // Character edit view back button
  document.getElementById('menu-char-edit-back').addEventListener('click', () => {
    showView('menu-model-view');
  });
  // Character edit: upload buttons for each state
  for (const state of ['sleeping', 'working', 'alert']) {
    document.getElementById('char-edit-' + state).addEventListener('click', async (e) => {
      e.stopPropagation();
      if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog('[char-edit] click state=' + state + ' editingCharId=' + editingCharId);
      if (!editingCharId || !window.petAPI || !window.petAPI.pickCharacterAnimation) return;
      const hint = document.getElementById(`char-edit-${state}-hint`);
      window.__PICKING_FILE__ = true;
      try {
        const result = await window.petAPI.pickCharacterAnimation(editingCharId, state);
        if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog('[char-edit] result=' + JSON.stringify(result));
        if (result && result.success) {
          // Show a brief "upload succeeded" flash before the hint settles
          // into the "已上传 / 重新上传" steady state.
          if (hint) {
            hint.innerHTML = '<span style="color: rgba(100, 220, 120, 0.95); font-weight: 600;">'
              + (i18n.t('menu.uploadSuccess') || '上传成功') + '</span>';
          }
          await loadCharacters();
          if (editingCharId === (activeCharacter && activeCharacter.id)) {
            refreshActiveCharacter();
          }
          setTimeout(() => { if (editingCharId) buildCharEditMenu(); }, 1200);
        }
      } catch (err) {
        const msg = err && err.message ? err.message : String(err);
        if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog('[char-edit] Upload failed: ' + msg);
        if (hint) {
          hint.innerHTML = '<span style="color: rgba(255, 100, 100, 0.9);">' + (i18n.t('menu.uploadFailed') || '上传失败') + '</span>';
          setTimeout(() => { if (editingCharId) buildCharEditMenu(); }, 2500);
        }
      } finally {
        window.__PICKING_FILE__ = false;
      }
    });
  }
  // Character edit: delete button
  document.getElementById('char-edit-delete').addEventListener('click', async () => {
    if (!editingCharId || !window.petAPI || !window.petAPI.deleteCharacter) return;
    await window.petAPI.deleteCharacter(editingCharId);
    editingCharId = null;
    await loadCharacters();
    showView('menu-model-view');
  });

  // Toggle actions (optimistic update + emit to main)
  document.getElementById('menu-flip').addEventListener('click', () => {
    flipHorizontal = !flipHorizontal;
    updateFlipMenuCheck();
    sendAction('toggleFlip');
  });
  document.getElementById('menu-mini-mode').addEventListener('click', () => {
    miniMode = !miniMode;
    document.getElementById('menu-mini-mode').classList.toggle('active', miniMode);
    sendAction('toggleMiniMode');
  });
  document.getElementById('menu-auto-launch').addEventListener('click', () => {
    const item = document.getElementById('menu-auto-launch');
    const newState = !item.classList.contains('active');
    item.classList.toggle('active', newState);
    sendAction('toggleAutoLaunch');
  });
  document.getElementById('menu-external-access').addEventListener('click', () => {
    const item = document.getElementById('menu-external-access');
    const newState = !item.classList.contains('active');
    item.classList.toggle('active', newState);
    sendAction('toggleExternalAccess');
  });

  // Actions that close the menu
  document.getElementById('menu-upload-live2d').addEventListener('click', () => {
    closeSelf();
    if (window.petAPI && window.petAPI.openLive2DFolder) {
      Promise.resolve(window.petAPI.openLive2DFolder()).catch(() => {});
    }
  });
  document.getElementById('menu-sounds-folder').addEventListener('click', () => {
    closeSelf();
    if (window.petAPI && window.petAPI.openSoundsFolder) {
      Promise.resolve(window.petAPI.openSoundsFolder()).catch(() => {});
    }
  });
  document.getElementById('menu-test-alert').addEventListener('click', () => {
    closeSelf();
    sendAction('testAlert');
  });
  document.getElementById('menu-install-hooks').addEventListener('click', () => {
    closeSelf();
    sendAction('installHooks');
  });
  document.getElementById('menu-hook-status').addEventListener('click', () => {
    closeSelf();
    sendAction('showHookStatus');
  });
  document.getElementById('menu-quit').addEventListener('click', () => {
    if (window.petAPI) window.petAPI.quit();
  });

  // Escape closes the menu
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      e.preventDefault();
      closeSelf();
    }
  });
}

/**
 * Fetch the character catalog and active choice from the main process, then
 * populate the "切换形象" menu.
 */
async function loadCharacters() {
  if (!window.petAPI || !window.petAPI.getCharacters) return;
  try {
    charactersData = await window.petAPI.getCharacters();
    // Determine active character object
    const all = [...(charactersData.builtin || []), ...(charactersData.custom || [])];
    activeCharacter = all.find(c => c.id === charactersData.active) || all[0] || null;
    if (activeCharacter && activeCharacter.type === 'live2d') {
      currentModelUrl = resolveLive2DUrl(activeCharacter);
    }
    buildCharacterGrid();
    // In menu mode, refit the window after the character grid rebuilds
    // (e.g. after creating/deleting a character while the menu is open).
    fitMenuWindow();
  } catch (err) {
    console.warn('[characters] Failed to load:', err.message);
  }
}

/**
 * Resolve a Live2D character's URL for use with Live2DModel.from().
 * Built-in models use relative URLs (assets/live2d/...). User-uploaded
 * models have absolute filesystem paths that must be converted to HTTP
 * server URLs (http://localhost:17521/live2d/...). We use "localhost"
 * instead of "127.0.0.1" because WebView2 may block fetch() to raw IP
 * loopback addresses. The HTTP server is needed (not convertFileSrc)
 * because pixi-live2d-display resolves relative paths (moc3, textures)
 * against the model3.json URL, and the asset protocol doesn't handle
 * relative path resolution correctly.
 */
function resolveLive2DUrl(char) {
  if (!char || !char.url) return null;
  // getCharacters (tauri-bridge.js) already converts user-uploaded model
  // paths to HTTP server URLs, so char.url is ready to use as-is.
  return char.url;
}

/**
 * Async URL resolver for Live2D models. Built-in models return their
 * relative URL directly. User-uploaded models have their URL already
 * converted to an HTTP server URL (http://localhost:17521/live2d/...) by
 * getCharacters in tauri-bridge.js, so we just return it as-is.
 *
 * Returns a URL suitable for `Live2DModel.from()`.
 */
async function resolveLive2DUrlAsync(char) {
  if (!char || !char.url) return null;
  return char.url;
}

/**
 * Build the character-switching grid from `charactersData`.
 */
function buildCharacterGrid() {
  const container = document.getElementById('character-grid');
  if (!container || !charactersData) return;
  container.innerHTML = '';
  const all = [...(charactersData.builtin || []), ...(charactersData.custom || [])];
  for (const char of all) {
    const card = document.createElement('div');
    card.className = 'char-card';
    if (char.id === charactersData.active) card.classList.add('active');
    // Thumbnail
    const thumb = document.createElement('div');
    thumb.className = 'char-thumb';
    if (char.type === 'animation' && char.animations && char.animations.sleeping) {
      const url = animAssetUrl(char.animations.sleeping, char.versions && char.versions.sleeping);
      thumb.style.backgroundImage = `url("${url}")`;
    } else if (char.thumbnail) {
      const url = window.__TAURI__.core.convertFileSrc(char.thumbnail);
      thumb.style.backgroundImage = `url("${url}")`;
    } else {
      thumb.textContent = char.name.charAt(0).toUpperCase();
    }
    card.appendChild(thumb);
    // Name
    const name = document.createElement('div');
    name.className = 'char-name';
    name.textContent = char.name;
    card.appendChild(name);
    // Edit button for custom characters
    if (char.type === 'animation') {
      const editBtn = document.createElement('div');
      editBtn.className = 'char-edit-btn';
      editBtn.textContent = '✎';
      editBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        showEditCharacterView(char.id);
      });
      card.appendChild(editBtn);
    }
    // Click to switch
    card.addEventListener('click', () => {
      if (char.id === charactersData.active) { closeMenu(); return; }
      handleSwitchCharacter(char.id);
    });
    container.appendChild(card);
  }
}

/**
 * Highlight the active model in the menu.
 */
function updateModelMenuActive(url) {
  // Refresh from backend after model switch
  loadCharacters();
}

/**
 * Wait for PixiJS and Live2D libraries to be loaded.
 */
function waitForLibs() {
  return new Promise((resolve) => {
    window.addEventListener('libs-ready', () => resolve(true), { once: true });
    window.addEventListener('libs-failed', () => resolve(false), { once: true });

    // If already loaded (script finished before listener attached)
    if (window.PIXI && window.PIXI.live2d) {
      resolve(true);
    }
  });
}

// ===== Animation Backend =====
// Unified animation interface. Upper-layer functions (playStateMotion,
// playMotionOnce, buildMotionMenu, updateHeadEffectAnchor, …) call animBackend
// methods ONLY — no `if (type === 'animation')` branches in call sites.
// Live2D vs custom-animation (GIF/MP4) differences are encapsulated here.

let animBackend = null;

/** Unified one-shot finish handler — called by whichever backend drives the
 *  restore (Live2D: motionFinish event; custom: setTimeout). Only clears the
 *  one-shot flag and restores the head effect; the caller resumes the state
 *  motion (Live2D needs a setTimeout deferral, custom does not). */
function onOneShotFinished() {
  if (!oneShotPlaying) return;
  oneShotPlaying = false;
  setHeadEffectVisible(true);
}

/** Reset all motion-playback state so switching characters / states / modes
 *  doesn't leave stale timers, flags, or hidden head effects.
 *  Always call this before re-initializing the animation layer. */
function resetMotionPlaybackState() {
  if (animBackend) animBackend.cancelPendingOneShot();
  oneShotPlaying = false;
  motionPreview = null;
  setHeadEffectVisible(true);
}

const live2dBackend = {
  type: 'live2d',

  play(group, idx, priority) {
    if (!live2dModel) return false;
    try {
      const r = live2dModel.motion(group, idx, priority);
      return r !== false && r !== undefined;
    } catch (e) { return false; }
  },

  playOneShot(group, idx) {
    // Restore is driven by the motionFinish event registered in attachModel.
    return this.play(group, idx, MOTION_PRIORITY_FORCE);
  },

  cancelPendingOneShot() { /* no-op: Live2D restore is event-driven */ },

  getMotionList() { return currentMotionGroups; },

  getMotionName(group, idx) { return motionDisplayName(group, idx); },

  getBounds() {
    if (!live2dModel || !pixiApp) return null;
    try {
      const b = live2dModel.getBounds();
      const r = pixiApp.view.getBoundingClientRect();
      return { x: b.x + r.left, y: b.y + r.top, width: b.width, height: b.height };
    } catch (e) { return null; }
  },

  isPlaying() {
    if (!live2dModel) return false;
    return live2dModel.internalModel.motionManager.playing;
  },

  onClick(cb) {
    if (live2dModel) live2dModel.on('pointerdown', cb);
  },

  setupHeadEffectTracking() {
    // Clean up the OTHER backend's tracking mechanism to prevent double registration.
    if (setupHeadEffect._timer) { clearInterval(setupHeadEffect._timer); setupHeadEffect._timer = null; }
    if (window.PIXI && pixiApp) {
      pixiApp.ticker.remove(updateHeadEffectAnchor);
      pixiApp.ticker.add(updateHeadEffectAnchor);
    }
  },
};

const customAnimBackend = {
  type: 'animation',

  play(group, idx, priority) {
    // group IS the state name for custom characters.
    updateCustomAnimation(group);
    return true;
  },

  playOneShot(group, idx) {
    const anims = (activeCharacter && activeCharacter.animations) || {};
    if (!anims[group]) return false;
    updateCustomAnimation(group);
    this.cancelPendingOneShot();
    customOneShotTimer = setTimeout(() => {
      customOneShotTimer = null;
      onOneShotFinished();
      playStateMotion(MOTION_PRIORITY_FORCE);
    }, 3000);
    return true;
  },

  cancelPendingOneShot() {
    if (customOneShotTimer) { clearTimeout(customOneShotTimer); customOneShotTimer = null; }
  },

  getMotionList() {
    if (!activeCharacter || !activeCharacter.animations) return [];
    return ['sleeping', 'working', 'alert']
      .filter(s => activeCharacter.animations[s])
      .map(s => [s, 1]);
  },

  getMotionName(group, idx) { return i18n.t('state.' + group) || group; },

  getBounds() {
    const el = customAnimEl || document.getElementById('canvas-wrapper');
    if (!el) return null;
    const r = el.getBoundingClientRect();
    return { x: r.left, y: r.top, width: r.width, height: r.height };
  },

  isPlaying() { return true; }, // GIF/MP4 loop on their own

  onClick(cb) {
    // Use 'click' instead of 'pointerdown' so the callback only fires on a
    // genuine tap (mousedown + mouseup without significant movement). This
    // prevents playRandomOneShot() from modifying the DOM (swapping the GIF)
    // during a drag-start, which could interfere with the mousedown → drag
    // event chain and break edge-dock snapping.
    if (customAnimEl) customAnimEl.addEventListener('click', cb);
  },

  setupHeadEffectTracking() {
    // Clean up the OTHER backend's tracking mechanism to prevent double registration.
    if (window.PIXI && pixiApp) pixiApp.ticker.remove(updateHeadEffectAnchor);
    if (setupHeadEffect._timer) clearInterval(setupHeadEffect._timer);
    setupHeadEffect._timer = setInterval(updateHeadEffectAnchor, 120);
  },
};

function selectAnimBackend() {
  if (activeCharacter && activeCharacter.type === 'animation') {
    animBackend = customAnimBackend;
    // Custom characters: the state name IS the motion group name.
    STATE_MOTIONS = {
      sleeping: ['sleeping', 0],
      working:  ['working', 0],
      alert:    ['alert', 0],
    };
  } else {
    animBackend = live2dBackend;
  }
  if (window.__petSendLog) window.__petSendLog('info',
    `[backend] selectAnimBackend: type=${activeCharacter ? activeCharacter.type : 'null'} → ${animBackend.type}, motions=${JSON.stringify(animBackend.getMotionList())}`);
}

// ===== Custom Animations (GIF/MP4 replacing Live2D) =====

/**
 * Build an asset-protocol URL for an animation file, appending a
 * cache-busting `?v=<mtime>` query. Re-uploading a GIF overwrites the file
 * in place (same filename), so without a versioned URL the browser serves
 * the stale cached image. The Tauri asset protocol ignores the query string
 * when resolving the file. `version` is the file mtime from `char.versions`.
 */
function animAssetUrl(path, version) {
  const base = window.__TAURI__.core.convertFileSrc(path);
  return version ? `${base}?v=${version}` : base;
}

function initCustomAnimation() {
  const container = document.getElementById('live2d-canvas');
  if (!container) return;
  customAnimEl = document.createElement('div');
  customAnimEl.id = 'custom-animation';
  // Block native HTML5 drag-and-drop on the entire custom animation container.
  // Without this, mousedown+drag on the img/video triggers the browser's
  // native file drag (image copy/download), which steals the mouse events
  // from our window-drag handler and breaks edge-dock snapping.
  customAnimEl.addEventListener('dragstart', (e) => e.preventDefault());
  container.appendChild(customAnimEl);
  if (pixiApp && pixiApp.view) {
    // Use opacity:0 instead of display:none so the PixiJS ticker keeps
    // running (rAF pauses when the canvas is display:none). This keeps
    // click-through region reporting alive AND preserves the WebGL context
    // for when the user switches back to a Live2D character.
    pixiApp.view.style.opacity = '0';
    pixiApp.view.style.pointerEvents = 'none';
  }
  // Hide the fallback canvas (shown when a Live2D model failed to load) so it
  // doesn't stack on top of the custom animation.
  const fb = document.getElementById('fallback-canvas');
  if (fb) fb.style.display = 'none';
  selectAnimBackend();
  updateCustomAnimation(currentState);
  // Apply the persisted horizontal-flip setting to the custom animation element
  // (initPixiApp normally does this, but GIF mode skips initPixiApp entirely).
  applyFlipCSS();
  // Set up the head-top effects (ZZZ / working dots / !) — same as attachModel
  // does for Live2D. setupHeadEffect uses setInterval when there's no pixiApp.
  setupHeadEffect();
  // Tap the pet -> play a random motion (unified via animBackend.onClick).
  animBackend.onClick(() => playRandomOneShot());
  // Register click-through regions (uses setInterval fallback when pixiApp is null).
  setupClickThrough();
}

function updateCustomAnimation(state) {
  if (!activeCharacter || activeCharacter.type !== 'animation' || !customAnimEl) return;
  const anims = activeCharacter.animations || {};
  const path = anims[state];
  if (!path) return;
  const versions = activeCharacter.versions || {};
  const url = animAssetUrl(path, versions[state]);
  const ext = path.split('.').pop().toLowerCase();
  const isVideo = ['mp4', 'webm', 'mov'].includes(ext);
  // Clear previous content.
  while (customAnimEl.firstChild) customAnimEl.removeChild(customAnimEl.firstChild);
  if (isVideo) {
    const video = document.createElement('video');
    video.src = url;
    video.autoplay = true;
    video.loop = true;
    video.muted = true;
    video.playsInline = true;
    // Disable native drag — otherwise dragging the video triggers a file
    // drag-and-drop operation that interferes with window dragging.
    video.draggable = false;
    customAnimEl.appendChild(video);
  } else {
    const img = document.createElement('img');
    img.src = url;
    img.alt = state;
    // Disable native drag — otherwise dragging the image triggers a file
    // drag-and-drop operation (image copy/download) that interferes with
    // window dragging and breaks edge-dock snapping.
    img.draggable = false;
    customAnimEl.appendChild(img);
  }
}

async function refreshActiveCharacter() {
  await loadCharacters();
  if (window.__MENU_MODE__) {
    // Menu window: forward to the main window so it refreshes rendering
    // (e.g., after re-uploading a GIF for the active character).
    if (window.__TAURI__ && window.__TAURI__.event) {
      window.__TAURI__.event.emit('menu-action', { action: 'refreshCharacter' });
    }
    return;
  }
  // Cancel any in-flight one-shot when the active character changes.
  resetMotionPlaybackState();
  if (activeCharacter && activeCharacter.type === 'animation') {
    if (!customAnimEl) initCustomAnimation();
    else { selectAnimBackend(); updateCustomAnimation(currentState); }
  } else if (customAnimEl) {
    customAnimEl.remove();
    customAnimEl = null;
    if (pixiApp && pixiApp.view) {
      pixiApp.view.style.opacity = '';
      pixiApp.view.style.pointerEvents = '';
    }
    // Always re-select backend and load the correct model when switching
    // from custom animation to Live2D — the previous model may belong to
    // a different character.
    selectAnimBackend();
    if (!live2dModel) initLive2D();
    else await switchModel(activeCharacter ? resolveLive2DUrl(activeCharacter) : null);
  }
}

// ===== Live2D Initialization =====
/**
 * Create the PixiJS application (once) and load the initial model.
 * The model URL order is: persisted choice → built-in MODEL_URLS fallbacks.
 */
async function initLive2D() {
  // If active character is animation-based, use custom animation instead of Live2D.
  if (activeCharacter && activeCharacter.type === 'animation') {
    initCustomAnimation();
    return;
  }
  try {
    initPixiApp();
    await loadInitialModel();
  } catch (err) {
    if (window.__petSendLog) window.__petSendLog('error', '[live2d] init FAILED: ' + (err && err.message ? err.message : err));
    live2dModel = null;
  }
}

/**
 * Create the PixiJS application and attach its canvas to the DOM.
 */
function initPixiApp() {
  if (pixiApp) return;
  const PIXI = window.PIXI;
  pixiApp = new PIXI.Application({
    width: miniMode ? MINI_PIXI_WIDTH : PIXI_WIDTH,
    height: miniMode ? MINI_PIXI_HEIGHT : PIXI_HEIGHT,
    backgroundAlpha: 0,
    antialias: true,
    // preserveDrawingBuffer allows canvas.toDataURL() to capture the rendered
    // frame for model thumbnails (without this, WebGL clears the buffer after
    // compositing and toDataURL returns a blank image). The perf cost is
    // negligible at 24fps on a small canvas.
    preserveDrawingBuffer: true,
    // Render at >=2x device pixels so edges stay crisp (the CSS display size
    // is unchanged — autoDensity keeps the canvas element at logical pixels).
    // NOTE: with resolution>1, pixiApp.view.width is the BACKING STORE size;
    // layout math must use pixiApp.screen (logical units) instead.
    resolution: Math.max(2, window.devicePixelRatio || 1),
    autoDensity: true,
    autoStart: true,
    // Prefer the discrete GPU on dual-GPU systems (common on laptops).
    powerPreference: 'high-performance',
  });
  const canvasContainer = document.getElementById('live2d-canvas');
  canvasContainer.appendChild(pixiApp.view);
  // Restore the persisted horizontal mirror on the freshly created canvas.
  applyFlipCSS();

  // Fixed 24fps cap — smooth enough for Live2D idle animations while keeping
  // CPU/GPU usage low (important over Remote Desktop where every frame adds
  // network bandwidth). No focus/blur switching needed.
  pixiApp.ticker.maxFPS = 24;

  // Register the click-through region reporter now that pixiApp exists (it
  // registers a PixiJS ticker callback). The _done guard makes re-entry safe.
  setupClickThrough();
}

/**
 * Try loading a model from a prioritized URL list, keeping the first that
 * succeeds. Used for the initial load (persisted choice first, then fallbacks).
 */
async function loadInitialModel() {
  const { Live2DModel } = window.PIXI.live2d;
  // Build candidate list: persisted choice first, then built-in fallbacks,
  // then any other catalog models — deduped, nulls filtered.
  const candidates = [
    currentModelUrl,
    ...MODEL_URLS,
    ...availableModels.map((m) => m.url),
  ].filter(Boolean);
  const seen = new Set();
  for (const url of candidates) {
    if (seen.has(url)) continue;
    seen.add(url);
    try {
      live2dModel = await Live2DModel.from(url);
      currentModelUrl = url;
      attachModel(live2dModel);
      // currentState is already 'sleeping' at init, so setPetState would be a
      // no-op — kick off the state motion directly.
      oneShotPlaying = false;
      playStateMotion(MOTION_PRIORITY_FORCE);
      return;
    } catch (err) {
      if (window.__petSendLog) window.__petSendLog('warn', '[live2d] model FAIL ' + url + ': ' + (err && err.message ? err.message : err));
    }
  }
  live2dModel = null;
}

/**
 * Measure the model's actual content bounds at UNIT SCALE (pixels) by
 * scanning every drawable's vertex positions. model.width/height come from
 * the moc3 CANVAS info, which can be far larger than the character itself
 * (e.g. shizuku ships a very wide canvas), so canvas-based fitting shrinks
 * such models to a tiny blob. Vertex bounds reflect what's actually visible.
 *
 * IMPORTANT: getDrawableVertexPositions returns CANVAS-UNIT coordinates
 * (canvas width = 1 unit), NOT pixels — multiply by internalModel
 * .pixelsPerUnit. Skipping that conversion yields a ~1x1 "bounds" and a
 * scale factor in the hundreds, blowing the model out of the window.
 *
 * Also only valid AFTER the core has updated at least once — right after
 * load the vertex buffers are uninitialized, which is why callers must
 * defer measurement to the first rendered frames.
 */
function measureContentBounds(model) {
  try {
    const im = model.internalModel;
    const core = im.coreModel;
    // Unit→pixel factor. pixelsPerUnit is provided by pixi-live2d-display;
    // fall back to deriving it from the canvas info.
    let ppu = im.pixelsPerUnit;
    if (!ppu || ppu <= 0) {
      const cw = core.getCanvasWidth ? core.getCanvasWidth() : 0;
      ppu = cw > 0 ? model.width / cw : 1;
    }
    const count = core.getDrawableCount();
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (let d = 0; d < count; d++) {
      // Vertex positions are already in MODEL coordinates (drawable-local
      // transforms were applied by the core during update), so no extra
      // matrix is needed here.
      const verts = core.getDrawableVertexPositions(d);
      if (!verts || verts.length === 0) continue;
      for (let i = 0; i < verts.length; i += 2) {
        const x = verts[i] * ppu, y = verts[i + 1] * ppu;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
      }
    }
    if (!isFinite(minX)) return null;
    return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
  } catch (e) {
    return null;
  }
}

/**
 * Capture a 128×128 PNG snapshot of the current Live2D model and save it to
 * ~/.dutyon/thumbnails/<name>.png via IPC, so the switch-character menu can
 * show a real preview instead of a letter avatar. Runs once per model name
 * per session (tracked in `capturedThumbnails`). Non-critical: any error is
 * silently swallowed — the menu just falls back to the letter avatar.
 */
const capturedThumbnails = new Set();
function captureThumbnail(model) {
  if (!activeCharacter || activeCharacter.type !== 'live2d') return;
  const name = activeCharacter.name;
  if (!name || capturedThumbnails.has(name)) return;
  if (!pixiApp || !pixiApp.view) return;
  if (!window.petAPI || !window.petAPI.saveModelThumbnail) return;
  try {
    // Force a render so the buffer is current (preserveDrawingBuffer keeps it).
    pixiApp.render();
    const src = pixiApp.view;
    // Downscale to 128×128 — the source may be high-DPI (backing store is
    // resolution×logical). drawImage handles the scaling.
    const thumb = document.createElement('canvas');
    thumb.width = 128;
    thumb.height = 128;
    const ctx = thumb.getContext('2d');
    ctx.drawImage(src, 0, 0, src.width, src.height, 0, 0, 128, 128);
    const dataUrl = thumb.toDataURL('image/png');
    capturedThumbnails.add(name);
    window.petAPI.saveModelThumbnail(name, dataUrl).catch(() => {});
  } catch (e) {
    // Non-critical — thumbnail is a nice-to-have.
  }
}

/**
 * Generate thumbnails for Live2D models that don't have one yet.
 *
 * Uses the main PIXI renderer's RenderTexture to render each model off-screen,
 * without adding it to the stage or hiding the current model. This avoids:
 * 1. Creating a second WebGL context (causes GPU driver to reclaim main context)
 * 2. Interfering with the current model's rendering (causes character switching
 *    issues when the user clicks during generation)
 *
 * The model is loaded, positioned, and rendered to a 128×128 RenderTexture
 * via `pixiApp.renderer.render(model, { renderTexture })`. The current pet
 * stays fully visible and interactive throughout.
 */
async function generateMissingThumbnails() {
  if (!charactersData || !window.PIXI || !window.PIXI.live2d || !pixiApp) return;
  const PIXI = window.PIXI;

  // Collect Live2D models that still need thumbnails.
  const allChars = [...(charactersData.builtin || []), ...(charactersData.custom || [])];
  const missing = allChars.filter(
    (c) => c.type === 'live2d' && !capturedThumbnails.has(c.name) && !c.thumbnail
  );
  if (missing.length === 0) return;

  const { Live2DModel } = PIXI.live2d;
  const dbg = (msg) => { if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog(msg); };

  // Create a single 128×128 RenderTexture for all thumbnails.
  const rt = PIXI.RenderTexture.create({ width: 128, height: 128 });

  dbg('[thumb] Starting generation for ' + missing.length + ' models');
  for (const char of missing) {
    let model = null;
    try {
      dbg('[thumb] Loading ' + char.name);
      const modelUrl = await resolveLive2DUrlAsync(char);
      model = await Live2DModel.from(modelUrl || char.url);
      dbg('[thumb] Loaded ' + char.name + ' w=' + model.width + ' h=' + model.height);

      // Fit the model into 128×128 (same 0.72 factor as attachModel).
      const scale = Math.min(128 / model.width, 128 / model.height) * 0.72;
      model.scale.set(scale);
      model.anchor.set(0.5, 1);
      model.x = 64;
      model.y = 128;

      // Wait for the model's core to initialize (vertex data is invalid
      // until the first few updates). 600ms ≈ 14 frames at 24fps.
      await new Promise((r) => setTimeout(r, 600));

      // Render the model to the RenderTexture (off-screen, doesn't affect
      // the main canvas or the current pet display).
      pixiApp.renderer.render(model, { renderTexture: rt, clear: true });

      // Extract as data URL.
      const canvas = pixiApp.renderer.extract.canvas(rt);
      const dataUrl = canvas.toDataURL('image/png');
      dbg('[thumb] Captured ' + char.name + ', dataUrl length=' + dataUrl.length);
      capturedThumbnails.add(char.name);
      if (window.petAPI && window.petAPI.saveModelThumbnail) {
        window.petAPI.saveModelThumbnail(char.name, dataUrl).catch(() => {});
      }
    } catch (e) {
      const msg = e && e.message ? e.message : String(e);
      dbg('[thumb] FAILED ' + char.name + ': ' + msg);
    } finally {
      // Do NOT call model.destroy() — pixi-live2d-display may cache the
      // model's Moc internally, and destroying it corrupts the cache. When
      // switchModel() later tries to load the same model via
      // Live2DModel.from(url), it gets the destroyed Moc and fails, causing
      // loadInitialModel() to fall back to nito (the "quickly switches back"
      // bug). Instead, just let the GC clean up the model object.
      model = null;
    }
  }
  dbg('[thumb] Generation complete');

  // Clean up the render texture.
  try { rt.destroy(true); } catch (e) { /* ignore */ }
}

/**
 * Scale, position, and wire up interaction/parameter hooks for a model.
 */
function attachModel(model) {
  // Initial fit by the moc3 CANVAS size — always valid right after load.
  // The tighter content-bounds fit is refined in refineContentFit() once the
  // core has rendered its first frame (vertex data is uninitialized before
  // then, so measuring earlier yields a bogus 1x1 box).
  let scale = Math.min(
    pixiApp.screen.width / model.width,
    pixiApp.screen.height / model.height
  ) * 0.72;

  // Display height at unit scale (≈ canvas height) — captured before we
  // apply our own scale below.
  const unscaledH = model.height;
  model.scale.set(scale);
  // NOTE: horizontal mirroring (左右翻转) is done via CSS on the canvas
  // element (applyFlipCSS) — a negative scale.x breaks pixi-live2d-display's
  // clipping masks and hides most of models that use them (e.g. shizuku).
  model.anchor.set(0.5, 1);
  model.x = pixiApp.screen.width / 2;
  model.y = pixiApp.screen.height;
  model._dutyonContent = null;      // filled in by refineContentFit
  model._dutyonUnscaledH = unscaledH;
  // Cache the base scale for mini-mode relayout: model.width/height are
  // scale-APPLIED (and animation-dependent) bounds, so recomputing from them
  // later would compound the scale on every toggle.
  model._dutyonBaseScale = scale;

  // One-shot content-bounds refinement after the first core update. The
  // core may need a couple of frames before vertex data is valid, so retry
  // briefly instead of giving up on the very first tick.
  let refineTries = 0;
  const refine = () => {
    if (live2dModel !== model || model.destroyed || ++refineTries > 60) {
      pixiApp.ticker.remove(refine);
      return;
    }
    const content = measureContentBounds(model);
    // Sanity: converted bounds are in pixels and should be comparable to the
    // canvas size — anything tiny means the conversion/measurement went
    // wrong, so keep the safe canvas fit instead.
    if (!content || content.width < 16 || content.height < 16) return;
    pixiApp.ticker.remove(refine);
    refineContentFit(model, content);
    // After the fit is applied, capture a thumbnail for the switch-character
    // menu (one snapshot per model name per session). Deferred one frame so
    // the new scale/position is rendered before capture.
    setTimeout(() => captureThumbnail(model), 200);
  };
  pixiApp.ticker.add(refine);

  pixiApp.stage.addChild(model);

  model.interactive = true;
  model.buttonMode = true;

  const mm = model.internalModel.motionManager;

  // Disable the library's built-in idle auto-play: on each motion finish it
  // picks a RANDOM idle motion, which would break "sleeping only plays 睡觉".
  // Pointing the idle group at a non-existent name makes that request a no-op;
  // we drive all playback ourselves via motionFinish + playStateMotion.
  mm.groups.idle = '__none__';

  // Loop the state motion seamlessly: whenever any motion ends, replay the
  // current state's motion (unless a one-shot is mid-flight — its own finish
  // will resume the state motion). Deferred via setTimeout so we run after the
  // library's same-tick complete()/idle bookkeeping.
  mm.on('motionFinish', () => {
    if (motionPreview) {
      // Hover preview: loop the motion until the preview is stopped. Deferred
      // via setTimeout (like the state-motion loop below) because the library
      // clears its motion slot after the emit — a synchronous replay here gets
      // rejected and the loop dies after one play.
      const [g, i] = motionPreview;
      if (!mm.destroyed) {
        setTimeout(() => {
          if (!motionPreview || mm.destroyed) return;
          const restart = () => {
            try { return live2dModel.motion(g, i, MOTION_PRIORITY_FORCE); }
            catch (e) { return false; }
          };
          // Returns false when the slot is still busy — retry once after a beat.
          if (restart() === false) {
            setTimeout(() => {
              if (motionPreview && !mm.destroyed) restart();
            }, 100);
          }
        }, 0);
      }
      return;
    }
    if (oneShotPlaying) {
      onOneShotFinished();
    }
    setTimeout(() => {
      if (!oneShotPlaying && !mm.destroyed) playStateMotion();
    }, 0);
  });

  // Tap the pet -> play a different motion once; the state motion resumes when
  // it finishes. Registered via the unified backend (same as custom animation).
  selectAnimBackend();
  animBackend.onClick(() => playRandomOneShot());

  // Set up the head-top effect (ZZZ / working dots / !) for the current state.
  setupHeadEffect();

  // Swap in this character's own saved state-motion settings (or the global
  // defaults on first use), then rebuild the motion catalog from its own
  // motion definitions — every character ships its own motion set, so the
  // menu and state-motion resolution must be refreshed on every load/switch.
  applyStateMotionsForModel(currentModelUrl);
  refreshMotionGroups();
  buildSettingsMenu();
}

/**
 * Re-fit the model to its actual content bounds (called once, after the
 * first core update). Canvases often carry large empty regions (e.g.
 * shizuku), so canvas fitting leaves such characters tiny or misaligned.
 *
 * Model coords are Y-up with the origin at canvas center; the display flips
 * Y, so the content bottom's display-local offset below the anchor point
 * (canvas bottom-center) is -(unscaledH/2 + content.y).
 */
function refineContentFit(model, content) {
  const unscaledH = model._dutyonUnscaledH || model.height;
  let scale = Math.min(
    pixiApp.screen.width / content.width,
    pixiApp.screen.height / content.height
  ) * 0.72;
  // Never let the content overflow the canvas horizontally (very wide
  // characters height-fitted would otherwise get clipped by the window).
  const maxScale = (pixiApp.screen.width * 1.05) / content.width;
  if (scale > maxScale) scale = maxScale;

  model._dutyonBaseScale = scale;
  model._dutyonContent = content;
  // Bottom-align on the CONTENT bottom instead of the canvas bottom.
  const bottomOffset = unscaledH / 2 + content.y; // display-local, >= 0
  model.scale.set(scale);
  model.x = pixiApp.screen.width / 2;
  model.y = pixiApp.screen.height + bottomOffset * scale;
}

/**
 * Per-frame: track the model's bounds and move #head-effect to the head-top
 * anchor. Registered via animBackend.setupHeadEffectTracking (PixiJS ticker
 * for Live2D, setInterval for custom animation). getBounds() accounts for
 * scale/anchor/animation, so the effect stays glued to the head as the
 * model breathes/moves.
 *
 * Both the downward inset and the effect size are derived from the bounds
 * height, so characters of very different proportions (small bundled models
 * vs large user-supplied ones) get the effect placed over the head instead
 * of buried in the body or floating in mid-air.
 */
function updateHeadEffectAnchor() {
  if (!headEffectEl) return;
  // Throttle: update every 3rd call (~20fps at 60fps ticker / 8fps at 120ms interval).
  updateHeadEffectAnchor._skip = ((updateHeadEffectAnchor._skip || 0) + 1) % 3;
  if (updateHeadEffectAnchor._skip !== 0) return;

  const b = animBackend ? animBackend.getBounds() : null;
  if (!b || b.height <= 0) return;
  const topX = b.x + b.width / 2;
  // Pick the anchor by body archetype, detected via the bounds aspect ratio:
  //  - Q-version chibi characters (aspect >= 0.8, head fills the top half of
  //    the bounds): keep the classic anchor ~20% below the bounds top
  //    (equivalent to the original fixed 40px on the reference 190px height).
  //  - Tall full-body characters (aspect < 0.8, e.g. Miku: head only occupies
  //    the top ~15% and hair/accessories push the bounds top up): anchor close
  //    to the bounds top and let the effect extend upward from the crown.
  const aspect = b.width / b.height;
  let inset;
  if (aspect < 0.8) {
    inset = Math.min(Math.max(b.height * 0.04, 4), 20);
  } else {
    inset = Math.min(Math.max(b.height * 0.2, 16), 48);
  }
  const topY = b.y + inset;
  // Effect scale: 1.0 at the reference height a typical bundled model renders
  // at (240×260 canvas × 0.72 fit ≈ 190px bounds). Smoothed to avoid jitter
  // as the bounds breathe with animation.
  const targetK = Math.min(Math.max(b.height / 190, 0.55), 1.8);
  const prevK = updateHeadEffectAnchor._k || targetK;
  const k = prevK + (targetK - prevK) * 0.15;
  updateHeadEffectAnchor._k = k;
  updateHeadEffectPosition(topX, topY, k);
}

/**
 * Create (or reset) the #head-effect container and seed it with the current
 * state's effect markup. Called from attachModel once the model is on stage.
 */
function setupHeadEffect() {
  headEffectEl = document.getElementById('head-effect');
  if (!headEffectEl) return;
  updateHeadEffectContent(currentState);
  // Reset the smoothed scale so a freshly loaded model snaps to its own size
  // instead of lerping from the previous model's scale.
  updateHeadEffectAnchor._k = 0;
  // Register per-frame anchor tracking via the backend (PixiJS ticker for
  // Live2D, setInterval for custom animation). Re-register on model switch
  // to avoid stacking listeners.
  if (animBackend) animBackend.setupHeadEffectTracking();
}

/**
 * Swap the inner markup of #head-effect based on state. Each effect is a
 * child anchored at bottom:0 (the 0×0 anchor point) extending upward ≤40px.
 */
function updateHeadEffectContent(state) {
  if (!headEffectEl) return;
  if (state === 'sleeping') {
    headEffectEl.innerHTML = '<div class="zzz-effect"><span>Z</span><span>Z</span><span>Z</span></div>';
  } else if (state === 'working') {
    headEffectEl.innerHTML = '<div class="working-effect"><span></span><span></span><span></span></div>';
  } else if (state === 'alert') {
    headEffectEl.innerHTML = '<div class="alert-effect">!</div>';
  } else {
    headEffectEl.innerHTML = '';
  }
}

/**
 * Move the #head-effect anchor to (x, y) in canvas-wrapper pixel coords and
 * scale it to match the model's on-screen size (see updateHeadEffectAnchor).
 */
function updateHeadEffectPosition(x, y, scale = 1) {
  if (!headEffectEl) return;
  headEffectEl.style.transform = `translate(${x}px, ${y}px) scale(${scale})`;
}

/**
 * Show/hide the head-top effect. Hidden while a tap-triggered one-shot motion
 * plays (so ZZZ/dots/! don't float above a non-state animation), restored when
 * the one-shot finishes (see motionFinish in attachModel).
 */
function setHeadEffectVisible(visible) {
  if (!headEffectEl) return;
  headEffectEl.classList.toggle('hidden', !visible);
}

/**
 * Switch to a different Live2D model at runtime. Destroys the current model,
 * loads the new one, persists the choice, and updates the menu.
 */
async function switchModel(url) {
  if (!window.PIXI || !window.PIXI.live2d) return;
  const { Live2DModel } = window.PIXI.live2d;
  if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog('[switch] switchModel START url=' + url);

  // Show immediate feedback so the user knows the click registered.
  const stateText = document.getElementById('pet-state-text');
  const prevText = stateText.textContent;
  stateText.textContent = i18n.t('model.switching');

  // Remove + destroy the current model (keep the PixiJS app for reuse)
  if (live2dModel) {
    pixiApp.stage.removeChild(live2dModel);
    try {
      live2dModel.destroy({ children: true, texture: true, baseTexture: true });
    } catch (e) { /* ignore cleanup errors */ }
    live2dModel = null;
  }

  try {
    live2dModel = await Live2DModel.from(url);
    currentModelUrl = url;
    if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog('[switch] Model loaded OK, attaching...');
    attachModel(live2dModel);
    // State hasn't changed (setPetState would no-op), so replay the state
    // motion on the new model directly.
    oneShotPlaying = false;
    playStateMotion(MOTION_PRIORITY_FORCE);
    updateModelMenuActive(url);
    // Persist the model choice — but only for built-in models (relative URLs).
    // User-uploaded models are loaded via HTTP server URLs (converted by
    // tauri-bridge.js); persisting that converted URL would overwrite the
    // original filesystem path that switchCharacter() already saved.
    // Skip persistence for HTTP-server / asset-protocol URLs.
    if (window.petAPI && window.petAPI.switchModel
        && !url.startsWith('http://asset.localhost/')
        && !url.startsWith('http://127.0.0.1:17521/')
        && !url.startsWith('http://localhost:17521/')) {
      window.petAPI.switchModel(url);
    }
  } catch (err) {
    const errMsg = (err && err.message) ? err.message : String(err);
    const errFrame = (err && err.stack) ? err.stack.split('\n').slice(0, 3).join(' <- ') : '';
    if (window.__petSendLog) window.__petSendLog('error', '[live2d] switch FAIL ' + url + ': ' + errMsg + (errFrame ? ' | ' + errFrame : ''));
    // Probe the same URL with fetch to capture the asset-protocol HTTP status
    // (403 = scope rejection, 404 = path mapping) plus the response head.
    try {
      const probe = await fetch(url);
      const probeBody = await probe.text();
      if (window.__petSendLog) window.__petSendLog('error', '[live2d] probe status=' + probe.status + ' len=' + probeBody.length + ' head=' + probeBody.slice(0, 120).replace(/\s+/g, ' '));
    } catch (probeErr) {
      if (window.__petSendLog) window.__petSendLog('error', '[live2d] probe threw: ' + ((probeErr && probeErr.message) || probeErr));
    }
    // Restore the previous model if the new one failed to load
    if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog('[switch] FAILED, calling loadInitialModel as fallback');
    await loadInitialModel();
    stateText.textContent = i18n.t('model.switchFailed');
    setTimeout(() => { stateText.textContent = prevText; }, 2000);
    return;
  }
  stateText.textContent = prevText;
}

/**
 * Per-state default motion: each state loops ONE specific motion.
 *   sleeping -> Flick3[1]      哈欠
 *   working  -> FlickLeft[1]   走路
 *   alert    -> FlickLeft[0]   yeah
 *
 * Per-state motion assignments are stored PER CHARACTER — see
 * applyStateMotionsForModel / saveCurrentStateMotions. DEFAULT_STATE_MOTIONS
 * only seeds characters that have no saved settings yet; STATE_MOTIONS is
 * the LIVE map for the currently loaded character.
 */
const DEFAULT_STATE_MOTIONS = {
  sleeping: ['Flick3', 1],       // 哈欠
  working:  ['FlickLeft', 1],    // 走路
  alert:    ['FlickLeft', 0],    // yeah
};
let STATE_MOTIONS = { ...DEFAULT_STATE_MOTIONS };

// Persisted per-model motion store: { [modelUrl]: { sleeping/working/alert:
// [group, idx] }, _default?: {...} }. null = not loaded yet.
let stateMotionsStore = null;

/**
 * Catalog of bundled-model motions with display names (group, index, i18nKey).
 * Used ONLY for display-name lookup and as a preferred playback order. The
 * actually playable motions always come from the loaded model itself (see
 * refreshMotionGroups), because every Live2D model ships its own motion set.
 */
const ALL_MOTIONS = [
  ['Idle', 0, 'motion.Idle.0'],
  ['Idle', 1, 'motion.Idle.1'],
  ['Idle', 2, 'motion.Idle.2'],
  ['Idle', 3, 'motion.Idle.3'],
  ['Tap', 0, 'motion.Tap.0'],
  ['Tap', 1, 'motion.Tap.1'],
  ['Tap', 2, 'motion.Tap.2'],
  ['Tap', 3, 'motion.Tap.3'],
  ['Tap', 4, 'motion.Tap.4'],
  ['FlickUp', 0, 'motion.FlickUp.0'],
  ['FlickUp', 1, 'motion.FlickUp.1'],
  ['FlickUp', 2, 'motion.FlickUp.2'],
  ['FlickDown', 0, 'motion.FlickDown.0'],
  ['FlickDown', 1, 'motion.FlickDown.1'],
  ['FlickRight', 0, 'motion.FlickRight.0'],
  ['Flick3', 0, 'motion.Flick3.0'],
  ['Flick3', 1, 'motion.Flick3.1'],
  ['FlickLeft', 0, 'motion.FlickLeft.0'],
  ['FlickLeft', 1, 'motion.FlickLeft.1'],
  ['Shake', 0, 'motion.Shake.0'],
  ['Shake', 1, 'motion.Shake.1'],
];

/**
 * Scan the loaded model's motion definitions and rebuild the available
 * motion list ([name, count] pairs, empty arrays dropped). Called from
 * attachModel so every model load/switch refreshes the catalog.
 */
function refreshMotionGroups() {
  currentMotionGroups = [];
  if (!live2dModel || !live2dModel.internalModel) return;
  const defs = live2dModel.internalModel.motionManager.definitions || {};
  for (const name of Object.keys(defs)) {
    const arr = defs[name];
    if (Array.isArray(arr) && arr.length > 0) {
      currentMotionGroups.push([name, arr.length]);
    }
  }
}

/** Count of playable motions in a group of the loaded model (0 if absent). */
function motionGroupCount(group) {
  const list = animBackend ? animBackend.getMotionList() : [];
  const found = list.find(([n]) => n === group);
  return found ? found[1] : 0;
}

/** Display name for a motion: i18n label when it's a known bundled motion,
 *  otherwise the raw `Group[idx]` form. */
function motionDisplayName(group, idx) {
  const found = ALL_MOTIONS.find(([mg, mi]) => mg === group && mi === idx);
  return found ? i18n.t(found[2]) : `${group}[${idx}]`;
}

/**
 * Resolve a desired motion to one the loaded backend can actually play:
 *   1. exact (group, idx) when the backend has it — same-name priority;
 *   2. same group, index 0 (the group exists but the index doesn't);
 *   3. any group that contains the desired one as a substring (Tap vs Tap2);
 *   4. the first available motion of the backend.
 * Returns [group, idx] or null when the backend has no motions at all.
 */
function resolveAvailableMotion(group, idx) {
  const list = animBackend ? animBackend.getMotionList() : [];
  if (!list.length) return null;
  const count = motionGroupCount(group);
  if (count > 0) {
    return [group, idx < count ? idx : 0];
  }
  const partial = list.find(([n]) => n.includes(group) || group.includes(n));
  if (partial) return [partial[0], 0];
  return [list[0][0], 0];
}

/**
 * Fallback chain for state motions when the resolved motion unexpectedly
 * fails to start: same group other indices -> bundled-catalog order -> any.
 */
function* fallbackMotionCandidates(group) {
  const count = motionGroupCount(group);
  for (let i = 0; i < count; i++) {
    if (i !== 0) yield [group, i];
  }
  for (const [g, i] of ALL_MOTIONS) {
    if (g !== group && motionGroupCount(g) > i) yield [g, i];
  }
  const list = animBackend ? animBackend.getMotionList() : [];
  for (const [n, c] of list) {
    if (n !== group) yield [n, 0];
  }
}

/**
 * Play the current state's motion so it loops. The desired motion comes from
 * STATE_MOTIONS (user-assignable) but is resolved against the backend's own
 * motion set; missing motions degrade to similar available ones.
 * `motionFinish` (Live2D) or `playOneShot` timeout (custom) re-triggers this
 * for seamless looping; it's also called directly on state changes and as a
 * periodic safety net.
 */
function playStateMotion(priority = MOTION_PRIORITY_NORMAL) {
  if (!animBackend) return;
  const desired = STATE_MOTIONS[currentState] || STATE_MOTIONS.sleeping;
  const target = resolveAvailableMotion(desired[0], desired[1]);
  if (!target) return;
  if (animBackend.play(target[0], target[1], priority)) return;
  // Playback failed — try fallbacks.
  for (const [g, i] of fallbackMotionCandidates(target[0])) {
    if (animBackend.play(g, i, priority)) {
      if (window.__petSendLog) window.__petSendLog('warn',
        `[motions] ${desired[0]}[${desired[1]}] failed, fell back to ${g}[${i}]`);
      return;
    }
  }
}

/**
 * Periodic safety net: if no motion is currently playing (e.g. motionFinish
 * was missed), replay the state motion. Skipped while a one-shot runs.
 */
function triggerPeriodicMotion() {
  if (!animBackend) return;
  if (oneShotPlaying) return;
  if (animBackend.isPlaying()) return;
  playStateMotion();
}

/**
 * Play a specific motion as a one-shot at FORCE priority (overrides the
 * looping state motion). Resolved against the backend's motion set first so
 * menu/assignment entries never point at motions the current character lacks.
 * When it finishes, the backend's finish handler (motionFinish / timeout)
 * resumes the state motion.
 */
function playMotionOnce(group, idx) {
  if (!animBackend) return;
  const target = resolveAvailableMotion(group, idx) || [group, idx];
  oneShotPlaying = true;
  setHeadEffectVisible(false);
  animBackend.cancelPendingOneShot();
  animBackend.playOneShot(target[0], target[1]);
}

/**
 * Start looping a motion as the hover preview of the 播放动作 menu. It plays
 * at FORCE priority and replays itself on every finish until stopMotionPreview()
 * is called (menu closed / list left).
 */
function startMotionPreview(group, idx) {
  if (!animBackend) return;
  animBackend.cancelPendingOneShot();
  const target = resolveAvailableMotion(group, idx) || [group, idx];
  motionPreview = target;
  oneShotPlaying = true; // blocks the state-motion safety net while previewing
  setHeadEffectVisible(false);
  animBackend.play(target[0], target[1], MOTION_PRIORITY_FORCE);
}

/**
 * End the hover preview and resume the current state's motion.
 */
function stopMotionPreview() {
  if (!motionPreview) return;
  motionPreview = null;
  oneShotPlaying = false;
  setHeadEffectVisible(true);
  playStateMotion(MOTION_PRIORITY_FORCE);
}

/**
 * Play a random motion different from the current state's (used on tap).
 * Picks from the backend's own motion set, not a global list.
 */
function playRandomOneShot() {
  if (!animBackend) return;
  const desired = STATE_MOTIONS[currentState] || STATE_MOTIONS.sleeping;
  const stateMotion = resolveAvailableMotion(desired[0], desired[1]);
  const choices = [];
  for (const [name, count] of animBackend.getMotionList()) {
    for (let i = 0; i < count; i++) {
      if (stateMotion && name === stateMotion[0] && i === stateMotion[1]) continue;
      choices.push([name, i]);
    }
  }
  if (!choices.length) return;
  const [group, idx] = choices[Math.floor(Math.random() * choices.length)];
  playMotionOnce(group, idx);
}

/**
 * Build the motion list in the menu from the backend's motion set, so each
 * character only shows what it can play.
 * - mode 'play'   : hovering (or clicking) an item loops that motion as a
 *                   live preview; the menu stays open — only the back button
 *                   or clicking outside closes it.
 * - mode 'assign' : hovering an item loops it as a live preview (so the user
 *                   can see the motion before committing); clicking assigns
 *                   it to `targetState` and returns to the 动作设定 view. The
 *                   current choice is checkmarked.
 */
function buildMotionMenu(mode = 'play', targetState = null) {
  const container = document.getElementById('motion-list');
  if (!container) return;
  container.innerHTML = '';

  const list = animBackend ? animBackend.getMotionList() : [];
  const desiredCurrent = (mode === 'assign' && targetState) ? STATE_MOTIONS[targetState] : null;
  const resolvedCurrent = desiredCurrent
    ? resolveAvailableMotion(desiredCurrent[0], desiredCurrent[1])
    : null;
  for (const [group, count] of list) {
    for (let idx = 0; idx < count; idx++) {
      const item = document.createElement('div');
      item.className = 'menu-item';
      item.textContent = animBackend.getMotionName(group, idx);
      if (resolvedCurrent && resolvedCurrent[0] === group && resolvedCurrent[1] === idx) {
        item.classList.add('active');
      }
      // Both modes share the hover preview: moving the cursor onto an item
      // instantly loops that motion (see startMotionPreview).
      item.addEventListener('mouseenter', () => startMotionPreview(group, idx));
      if (mode === 'assign' && targetState) {
        // Click commits the assignment and returns to the 动作设定 view
        // (showView stops the preview and resumes the state motion).
        item.addEventListener('click', () => {
          assignStateMotion(targetState, group, idx);
          showView('menu-settings-view');
        });
      } else {
        // Play mode: clicking just keeps the preview looping — only the back
        // button or clicking outside closes the menu.
        item.addEventListener('click', () => startMotionPreview(group, idx));
      }
      container.appendChild(item);
    }
  }
}

/**
 * Open the motion submenu in either play or assign mode.
 */
function openMotionView(mode, targetState = null) {
  motionPickerMode = mode;
  motionPickerTarget = targetState;
  buildMotionMenu(mode, targetState);
  const label = document.getElementById('motion-view-label');
  if (label) label.textContent = (mode === 'assign') ? i18n.t('menu.selectMotion') : i18n.t('menu.playMotion');
  showView('menu-motion-view');
}

/**
 * True when the value looks like a valid [group, index] motion entry.
 */
function isValidMotionEntry(m) {
  return Array.isArray(m) && m.length === 2 && typeof m[0] === 'string' && Number.isInteger(m[1]);
}

/**
 * Shallow-clone a state→motion map, keeping only valid entries.
 */
function cloneStateMotions(src) {
  const out = {};
  for (const state of ['sleeping', 'working', 'alert']) {
    if (src && isValidMotionEntry(src[state])) out[state] = [src[state][0], src[state][1]];
  }
  return out;
}

/**
 * Load persisted per-character motion assignments from the main process.
 *
 * Storage layout: { [modelUrl]: {sleeping/working/alert}, _default?: {...} }
 * The legacy FLAT format ({sleeping/working/alert} directly, shared by all
 * characters) is auto-migrated into `_default` and rewritten on first save.
 */
async function loadStateMotions() {
  if (!window.petAPI || !window.petAPI.getStateMotions) return;
  try {
    const saved = await window.petAPI.getStateMotions();
    const store = {};
    if (saved && typeof saved === 'object') {
      const states = ['sleeping', 'working', 'alert'];
      const isLegacy = states.some((s) => Array.isArray(saved[s]));
      if (isLegacy) {
        // Pre-per-model config — treat it as the shared default so every
        // character starts from what the user had before.
        store._default = cloneStateMotions(saved);
      } else {
        for (const [key, entry] of Object.entries(saved)) {
          if (!entry || typeof entry !== 'object') continue;
          const cleaned = cloneStateMotions(entry);
          if (Object.keys(cleaned).length > 0) store[key] = cleaned;
        }
      }
    }
    stateMotionsStore = store;
    // Persist the migrated layout so the legacy blob is not re-parsed on
    // every launch (harmless no-op when nothing changed).
    if (window.petAPI.setStateMotions) window.petAPI.setStateMotions(store);
  } catch (err) {
    console.warn('[motions] Failed to load state motions:', err.message);
  }
}

/**
 * Swap the live STATE_MOTIONS map to the saved settings of the given
 * character (called on every model load/switch). Resolution order:
 * character-specific → saved global default (legacy) → built-in defaults.
 */
function applyStateMotionsForModel(modelUrl) {
  const store = stateMotionsStore || {};
  const saved = (modelUrl && store[modelUrl]) || store._default || null;
  const next = cloneStateMotions(DEFAULT_STATE_MOTIONS);
  if (saved) {
    for (const state of ['sleeping', 'working', 'alert']) {
      if (saved[state]) next[state] = [saved[state][0], saved[state][1]];
    }
  }
  STATE_MOTIONS = next;
}

/**
 * Persist the live STATE_MOTIONS under the CURRENT character's key only —
 * other characters' saved settings are left untouched.
 */
function saveCurrentStateMotions() {
  if (!currentModelUrl) return;
  if (!stateMotionsStore) stateMotionsStore = {};
  stateMotionsStore[currentModelUrl] = cloneStateMotions(STATE_MOTIONS);
  if (window.petAPI && window.petAPI.setStateMotions) {
    window.petAPI.setStateMotions(stateMotionsStore);
  }
}

/**
 * Populate the 动作设定 view with each state's current motion name, resolved
 * to what the loaded model can play (shows the fallback target when the
 * desired motion doesn't exist on the current character).
 */
function buildSettingsMenu() {
  for (const state of ['sleeping', 'working', 'alert']) {
    const el = document.getElementById(`settings-${state}-name`);
    if (!el) continue;
    const desired = STATE_MOTIONS[state] || STATE_MOTIONS.sleeping;
    const resolved = resolveAvailableMotion(desired[0], desired[1]);
    el.textContent = resolved
      ? animBackend.getMotionName(resolved[0], resolved[1])
      : `${desired[0]}[${desired[1]}]`;
  }
}

function buildCharEditMenu() {
  if (!editingCharId || !charactersData) return;
  const char = [...(charactersData.builtin || []), ...(charactersData.custom || [])]
    .find(c => c.id === editingCharId);
  if (!char) return;
  const title = document.getElementById('char-edit-title');
  if (title) title.textContent = char.name;
  for (const state of ['sleeping', 'working', 'alert']) {
    const hint = document.getElementById(`char-edit-${state}-hint`);
    if (!hint) continue;
    const hasAnim = char.animations && char.animations[state];
    if (hasAnim) {
      hint.innerHTML =
        '<span class="char-uploaded-tag">' + (i18n.t('menu.uploaded') || '已上传') + '</span>' +
        '<span class="char-reupload-tag">' + (i18n.t('menu.reupload') || '重新上传') + '</span>';
    } else {
      hint.textContent = i18n.t('menu.upload') || '上传';
    }
  }
}

function showEditCharacterView(id) {
  editingCharId = id;
  buildCharEditMenu();
  showView('menu-char-edit-view');
}

/** Show the inline name-input row below "+ 新建形象". */
function handleCreateCharacter() {
  const inline = document.getElementById('char-create-inline');
  if (!inline) return;
  const input = document.getElementById('char-create-input');
  if (input) input.value = '';
  inline.classList.remove('hidden');
  if (input) setTimeout(() => input.focus(), 30);
  fitMenuWindow();
}

/** Confirm character creation from the inline input. */
async function confirmCreateCharacter() {
  const input = document.getElementById('char-create-input');
  const name = input ? input.value.trim() : '';
  if (!name) return;
  if (!window.petAPI || !window.petAPI.createCharacter) return;
  const result = await window.petAPI.createCharacter(name);
  // Always hide the inline input after confirming (success or failure).
  const inline = document.getElementById('char-create-inline');
  if (inline) inline.classList.add('hidden');
  if (result && result.id) {
    await loadCharacters();
    showEditCharacterView(result.id);
  }
}

async function handleSwitchCharacter(id) {
  if (!window.petAPI || !window.petAPI.switchCharacter) return;
  if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog('[switch] handleSwitchCharacter id=' + id + ' menuMode=' + !!window.__MENU_MODE__);
  await window.petAPI.switchCharacter(id);
  await loadCharacters();
  // IMPORTANT: loadCharacters() reads the config from disk. If the config
  // write failed (e.g., sandbox blocking file access, read-only directory),
  // activeCharacter would be the OLD character, not the one the user clicked.
  // Override with the character matching the clicked id so switchModel loads
  // the correct model regardless of config persistence state.
  const allChars = [...(charactersData.builtin || []), ...(charactersData.custom || [])];
  const clicked = allChars.find(c => c.id === id);
  if (clicked) {
    activeCharacter = clicked;
    if (clicked.type === 'live2d') {
      currentModelUrl = resolveLive2DUrl(clicked);
    }
  }
  if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog('[switch] activeCharacter=' + (activeCharacter ? activeCharacter.name : 'null') + ' url=' + (activeCharacter ? activeCharacter.url : 'null'));
  // Menu window: forward to main window for rendering, then close.
  if (window.__MENU_MODE__) {
    const { emit } = window.__TAURI__.event;
    emit('menu-action', { action: 'switchCharacter', params: { id } });
    emit('menu-close', {});
    if (window.petAPI && window.petAPI.hideMenuWindow) window.petAPI.hideMenuWindow();
    return;
  }
  // Reset any in-flight one-shot / preview from the OLD character so stale
  // timers and flags don't leak into the new character.
  resetMotionPlaybackState();
  // Re-init rendering
  if (activeCharacter && activeCharacter.type === 'animation') {
    if (!customAnimEl) initCustomAnimation();
    else { selectAnimBackend(); updateCustomAnimation(currentState); }
  } else {
    // Switching from GIF/custom to Live2D: tear down custom animation DOM,
    // restore pixiApp visibility (if it exists), and load the Live2D model.
    if (customAnimEl) {
      customAnimEl.remove();
      customAnimEl = null;
    }
    if (pixiApp && pixiApp.view) {
      pixiApp.view.style.opacity = '';
      pixiApp.view.style.pointerEvents = '';
    }
    // CRITICAL: switch animBackend back to live2dBackend BEFORE any motion
    // calls. Without this, animBackend stays as customAnimBackend and all
    // state/motion functions route to the wrong backend.
    selectAnimBackend();
    // If pixiApp was never created (app started in GIF mode), initLive2D()
    // creates it AND loads the model. Otherwise, switch to the new model.
    if (!pixiApp || !live2dModel) {
      await initLive2D();
    } else {
      await switchModel(activeCharacter ? resolveLive2DUrl(activeCharacter) : null);
    }
  }
  closeMenu();
}

/**
 * Assign a motion to a state, persist it, refresh the settings view, and if
 * the changed state is currently active, immediately switch to the new motion.
 */
function assignStateMotion(state, group, idx) {
  STATE_MOTIONS[state] = [group, idx];
  buildSettingsMenu();
  saveCurrentStateMotions();
  if (state === currentState) {
    oneShotPlaying = false;
    playStateMotion(MOTION_PRIORITY_FORCE);
  }
}

/**
 * Load persisted appearance settings (horizontal flip) from the main process.
 */
async function loadAppearance() {
  if (!window.petAPI || !window.petAPI.getAppearance) return;
  try {
    const { flipHorizontal: f, miniMode: m } = await window.petAPI.getAppearance();
    flipHorizontal = !!f;
    // No window resize here: the backend already created the window with the
    // persisted mini-mode size (see position_window in lib.rs).
    applyMiniMode(!!m, false);
  } catch (err) {
    console.warn('[appearance] Failed to load:', err.message);
  }
}

/**
 * Mirror the rendered canvas via CSS instead of a negative model scale.
 * A negative scale.x flips the winding of clipping-mask geometry inside
 * pixi-live2d-display and makes masked drawables invisible (models like
 * shizuku collapse to just their unmasked mouth). CSS mirroring happens
 * after WebGL compositing, so masks keep working. The canvas is centered
 * in its wrapper, so scaleX(-1) mirrors in place with no position jump.
 */
function applyFlipCSS() {
  const transform = flipHorizontal ? 'scaleX(-1)' : '';
  if (pixiApp && pixiApp.view) pixiApp.view.style.transform = transform;
  if (customAnimEl) customAnimEl.style.transform = transform;
}

/**
 * Toggle horizontal mirror, persist it, and update the menu checkmark.
 */
function toggleFlip() {
  flipHorizontal = !flipHorizontal;
  applyFlipCSS();
  updateFlipMenuCheck();
  if (window.petAPI && window.petAPI.setFlipHorizontal) {
    window.petAPI.setFlipHorizontal(flipHorizontal);
  }
}

/**
 * Show/hide the ✓ on the 左右翻转 menu item.
 */
function updateFlipMenuCheck() {
  const item = document.getElementById('menu-flip');
  if (item) item.classList.toggle('active', flipHorizontal);
}

/**
 * Recompute the model's scale/position after the canvas size changes
 * (mini-mode toggle). Geometry only — no event re-registration (attachModel).
 * Uses the base scale cached by attachModel scaled by the canvas ratio
 * (normal 240×260 ↔ mini 120×130 is exactly ×2/×0.5).
 */
function relayoutModel() {
  if (!live2dModel || !pixiApp) return;
  const base = live2dModel._dutyonBaseScale;
  if (!base) return;
  const ratio = Math.min(
    pixiApp.screen.width / PIXI_WIDTH,
    pixiApp.screen.height / PIXI_HEIGHT
  );
  const scale = base * ratio;
  live2dModel.scale.set(scale);
  live2dModel.x = pixiApp.screen.width / 2;
  // Keep the content-bottom alignment from refineContentFit when present.
  const content = live2dModel._dutyonContent;
  const unscaledH = live2dModel._dutyonUnscaledH || live2dModel.height;
  live2dModel.y = content
    ? pixiApp.screen.height + (unscaledH / 2 + content.y) * scale
    : pixiApp.screen.height;
}

/**
 * Apply mini mode: body CSS class + PixiJS canvas resize + optional window
 * resize through the backend. resizeWindow is false at startup because the
 * window already opens at the persisted size.
 */
function applyMiniMode(enabled, resizeWindow = true) {
  const prevMode = miniMode;
  miniMode = enabled;
  // The menu window is a separate popup — it should always render at full
  // size regardless of the pet's mini mode. Only apply the CSS class + canvas
  // resize in the main pet window.
  if (!window.__MENU_MODE__) {
    document.body.classList.toggle('mini', enabled);
  }
  updateMiniMenuCheck();
  if (resizeWindow && window.petAPI && window.petAPI.setMiniMode) {
    Promise.resolve(window.petAPI.setMiniMode(enabled)).catch((e) => {
      console.warn('[mini] Failed to resize window, rolling back:', e && e.message);
      // The window wasn't resized — restore the previous CSS class and canvas
      // so they stay consistent with the actual window size.
      miniMode = prevMode;
      document.body.classList.toggle('mini', prevMode);
      updateMiniMenuCheck();
      if (pixiApp) {
        const w = prevMode ? MINI_PIXI_WIDTH : PIXI_WIDTH;
        const h = prevMode ? MINI_PIXI_HEIGHT : PIXI_HEIGHT;
        if (pixiApp.screen.width !== w || pixiApp.screen.height !== h) {
          pixiApp.renderer.resize(w, h);
        }
        relayoutModel();
      }
    });
    // If the menu window is open, re-position it for the new pet size.
    if (menuWindowOpen) {
      closeMenu();
      positionMenu();
    }
  }
  if (pixiApp) {
    const w = enabled ? MINI_PIXI_WIDTH : PIXI_WIDTH;
    const h = enabled ? MINI_PIXI_HEIGHT : PIXI_HEIGHT;
    if (pixiApp.screen.width !== w || pixiApp.screen.height !== h) {
      pixiApp.renderer.resize(w, h);
    }
    relayoutModel();
  }
}

/**
 * Toggle mini mode and persist it (窗口尺寸由后端调整).
 */
function toggleMiniMode() {
  applyMiniMode(!miniMode, true);
}

/**
 * Show/hide the ✓ on the 迷你模式 menu item.
 */
function updateMiniMenuCheck() {
  const item = document.getElementById('menu-mini-mode');
  if (item) item.classList.toggle('active', miniMode);
}
// ===== Fallback Canvas Animation =====
function initFallbackCanvas() {
  const canvas = document.getElementById('fallback-canvas');
  canvas.style.display = 'block';
  const ctx = canvas.getContext('2d');

  let frame = 0;

  function drawFallback() {
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    const cx = canvas.width / 2;
    const cy = canvas.height / 2;
    const breathY = Math.sin(frame * 0.03) * 3;

    // Body (rounded rectangle)
    ctx.fillStyle = getStateColor(currentState);
    ctx.beginPath();
    ctx.roundRect(cx - 60, cy - 40 + breathY, 120, 140, 30);
    ctx.fill();

    // Head (circle)
    ctx.beginPath();
    ctx.arc(cx, cy - 60 + breathY, 50, 0, Math.PI * 2);
    ctx.fill();

    // Eyes
    if (currentState === 'sleeping') {
      // Closed eyes (lines)
      ctx.strokeStyle = '#333';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(cx - 18, cy - 65 + breathY, 8, 0, Math.PI, false);
      ctx.arc(cx + 18, cy - 65 + breathY, 8, 0, Math.PI, false);
      ctx.stroke();
    } else {
      // Open eyes (circles)
      ctx.fillStyle = '#333';
      const eyeSize = currentState === 'alert' ? 6 : 4;
      ctx.beginPath();
      ctx.arc(cx - 18, cy - 65 + breathY, eyeSize, 0, Math.PI * 2);
      ctx.arc(cx + 18, cy - 65 + breathY, eyeSize, 0, Math.PI * 2);
      ctx.fill();
    }

    // Mouth
    ctx.strokeStyle = '#333';
    ctx.lineWidth = 2;
    ctx.beginPath();
    if (currentState === 'alert') {
      // O mouth
      ctx.arc(cx, cy - 40 + breathY, 8, 0, Math.PI * 2);
    } else if (currentState === 'sleeping') {
      // Small line
      ctx.moveTo(cx - 8, cy - 40 + breathY);
      ctx.lineTo(cx + 8, cy - 40 + breathY);
    } else {
      // Slight smile
      ctx.arc(cx, cy - 45 + breathY, 10, 0.2, Math.PI - 0.2);
    }
    ctx.stroke();

    // Cheeks (blush)
    if (currentState !== 'sleeping') {
      ctx.fillStyle = 'rgba(255, 150, 150, 0.3)';
      ctx.beginPath();
      ctx.arc(cx - 28, cy - 50 + breathY, 8, 0, Math.PI * 2);
      ctx.arc(cx + 28, cy - 50 + breathY, 8, 0, Math.PI * 2);
      ctx.fill();
    }

    frame++;
    requestAnimationFrame(drawFallback);
  }

  drawFallback();
}

function getStateColor(state) {
  return STATE_COLORS[state] || STATE_COLORS.sleeping;
}

// ===== State Management =====
function setPetState(state) {
  if (currentState === state) return;
  currentState = state;

  // Cancel any in-flight one-shot / preview and immediately play the new
  // state's motion at FORCE priority so it overrides whatever was playing.
  // Unified via animBackend — works for Live2D and custom.
  resetMotionPlaybackState();
  playStateMotion(MOTION_PRIORITY_FORCE);

  // Update UI
  updateStateUI(state);

  // Swap the head-top effect (ZZZ / working dots / !) to match the new state.
  updateHeadEffectContent(state);
}

function updateStateUI(state) {
  const stateText = document.getElementById('pet-state-text');
  const statusBar = document.getElementById('status-bar');
  const canvasWrapper = document.getElementById('canvas-wrapper');

  // Remove old state classes
  stateText.className = '';
  statusBar.className = '';
  canvasWrapper.classList.remove('shake');

  // Add new state classes
  stateText.classList.add(`state-${state}`);
  statusBar.classList.add(`state-${state}`);

  stateText.textContent = getStateLabel(state);

  // Shake on alert — only for Live2D models. For GIF/MP4 custom animations,
  // shaking the canvas-wrapper shakes the entire image which looks bad;
  // the head-top "!" already has its own bounce animation.
  if (state === 'alert' && !(activeCharacter && activeCharacter.type === 'animation')) {
    canvasWrapper.classList.add('shake');
  }
}

// ===== Effects Loop =====
function startEffectsLoop() {
  if (effectsTimer) clearInterval(effectsTimer);
  effectsTimer = setInterval(() => {
    // Safety net: keep the state motion looping even if motionFinish is missed.
    triggerPeriodicMotion();
    // Head-top effects (ZZZ / working dots / !) are CSS animations inside
    // #head-effect, repositioned on the ticker by updateHeadEffectAnchor.
  }, EFFECT_INTERVAL_MS);
}

// ===== Status Bar Rendering =====
function updateStatusBar(snapshot) {
  currentSnapshot = snapshot;
  const projectList = document.getElementById('project-list');

  if (!snapshot.sessions || snapshot.sessions.length === 0) {
    projectList.innerHTML = `<div class="project-item empty">${i18n.t('status.waiting')}</div>`;
    renderEdgeDockBadges();
    repositionMenu(); // status bar height may have changed while the menu is open
    return;
  }

  projectList.innerHTML = '';
  for (const session of snapshot.sessions) {
    const item = document.createElement('div');
    item.className = `project-item status-${session.status}`;

    const dot = document.createElement('div');
    dot.className = `project-dot ${session.status}`;

    const name = document.createElement('div');
    // Alert sessions: keep the project name visible (highlighted red)
    // instead of letting the long alert text squeeze it out.
    name.className = 'project-name' + (session.status === 'confirmation-needed' ? ' alert' : '');
    name.textContent = session.projectName;
    const ideName = session.ide === 'qoder' ? 'Qoder' : session.ide === 'trae' ? 'Trae CN' : session.ide === 'cursor' ? 'Cursor' : session.ide === 'codex' ? 'Codex' : session.ide === 'opencode' ? 'OpenCode' : '';
    name.title = (ideName ? `[${ideName}] ` : '') + (session.projectPath || session.projectName);

    const statusText = document.createElement('div');
    statusText.className = 'project-status-text';
    if (session.status === 'confirmation-needed') {
      statusText.classList.add('alert');
      // Short label only; the full alert message moves to the tooltip so it
      // never displaces the project name.
      statusText.textContent = i18n.t('status.confirmationNeeded');
      if (session.alertMessage) statusText.title = session.alertMessage;
    } else if (session.status === 'thinking') {
      statusText.textContent = i18n.t('status.thinking');
    } else if (session.status === 'tool-use') {
      statusText.textContent = i18n.t('status.toolUse');
    } else if (session.status === 'working') {
      statusText.textContent = i18n.t('status.busy');
    } else {
      statusText.textContent = i18n.t('status.idle');
    }

    item.appendChild(dot);

    // IDE badge (T = Trae, Q = Qoder, C = Cursor, X = Codex, O = OpenCode)
    // when the source IDE is known. Codex shares the "C" initial with Cursor,
    // so its badge shows "X" (the distinctive ending); the tooltip still
    // reads "Codex". OpenCode's "O" is unique, so it uses the first letter.
    if (session.ide) {
      const ide = document.createElement('div');
      ide.className = `project-ide ide-${session.ide}`;
      const ideLabel = session.ide === 'qoder' ? 'Qoder' : session.ide === 'cursor' ? 'Cursor' : session.ide === 'codex' ? 'Codex' : session.ide === 'opencode' ? 'OpenCode' : 'Trae CN';
      ide.textContent = session.ide === 'codex' ? 'X' : ideLabel.charAt(0);
      ide.title = ideLabel;
      item.appendChild(ide);
    }

    item.appendChild(name);
    item.appendChild(statusText);

    // Click to bring IDE to front
    item.addEventListener('click', () => {
      if (!window.petAPI) return;
      // Window-detected sessions have no projectPath; fall back to projectName
      // (bring-to-front matches by folder name in the window title).
      const target = session.projectPath || session.projectName;
      if (target) window.petAPI.bringToFront(target);
    });

    projectList.appendChild(item);
  }
  renderEdgeDockBadges();
  repositionMenu(); // status bar height may have changed while the menu is open
}

// ===== IPC Setup =====
function setupIPC() {
  if (!window.petAPI) {
    console.warn('[IPC] petAPI not available (bridge not loaded)');
    return;
  }

  window.petAPI.onStateUpdate((snapshot) => {
    updateStatusBar(snapshot);
    // Refresh the Hook 状态 hint with the latest last-event time.
    updateHookStatusHint(hooksInstalled, snapshot);
    if (snapshot.overallState !== currentState) {
      setPetState(snapshot.overallState);
    }
  });

  // DPI scale changed at runtime (remote-desktop connect/disconnect, monitor
  // or display-settings switch). The backend already re-pinned the window to
  // its base logical size and dropped its menu-growth record — mirror that
  // locally: forget the grown space and close the menu if it was open.
  if (window.petAPI && window.petAPI.onDisplayChanged) {
    window.petAPI.onDisplayChanged(async () => {
      closeMenu();
      // Re-sync mini mode: the backend resizes the window on DPI change
      // based on the persisted config, but the frontend's CSS class and
      // canvas size might be out of sync if a setMiniMode IPC call failed
      // earlier or the window was moved between monitors with different DPI.
      if (window.petAPI && window.petAPI.getAppearance) {
        try {
          const { miniMode: m } = await window.petAPI.getAppearance();
          if (!!m !== miniMode) {
            applyMiniMode(!!m, false);
          }
        } catch (e) { /* cosmetic — best effort */ }
      }
    });
  }

  window.petAPI.onAlert((snapshot) => {
    updateStatusBar(snapshot);
    setPetState('alert');
    // Flash window attention
    flashWindowAttention();
  });

  // ===== Menu window event handlers =====
  // The separate menu window sends actions here for the main window to
  // process (play motion on the pet, toggle settings, etc.).
  const { listen } = window.__TAURI__.event;
  listen('menu-action', (e) => {
    const { action, params } = e.payload || {};
    if (window.petAPI && window.petAPI.debugLog) window.petAPI.debugLog('[menu-action] received action=' + action);
    switch (action) {
      case 'toggleFlip': toggleFlip(); break;
      case 'toggleMiniMode': toggleMiniMode(); break;
      case 'toggleAutoLaunch': toggleAutoLaunch(); break;
      case 'toggleExternalAccess': toggleExternalAccess(); break;
      case 'switchModel':
        if (params && params.url) {
          switchModel(params.url);
        }
        break;
      case 'switchCharacter':
        if (params && params.id) {
          handleSwitchCharacter(params.id);
        }
        break;
      case 'switchLanguage':
        // Already persisted by the menu window; just re-apply translations.
        applyTranslations();
        break;
      case 'playMotion':
        if (params) playMotionOnce(params.group, params.idx);
        break;
      case 'previewMotion':
        if (params) startMotionPreview(params.group, params.idx);
        break;
      case 'stopPreview': stopMotionPreview(); break;
      case 'assignMotion':
        if (params) assignStateMotion(params.state, params.group, params.idx);
        break;
      case 'testAlert':
        if (window.petAPI && window.petAPI.testAlert) window.petAPI.testAlert();
        break;
      case 'installHooks':
        (async () => {
          const before = await window.petAPI.isHooksInstalled();
          const result = await window.petAPI.installHooks();
          if (result.success) {
            showInstallResult(true, result, !!(before && before.installed));
            checkHooksStatus();
          } else {
            showInstallResult(false, result);
          }
        })();
        break;
      case 'showHookStatus':
        (async () => {
          const status = await window.petAPI.isHooksInstalled();
          showHookStatusDialog(status);
        })();
        break;
      case 'refreshCharacter':
        refreshActiveCharacter();
        break;
      default:
        console.warn('[menu] Unknown action from menu window:', action);
    }
  });

  // Menu window closed by user (Escape, back button, or action that closes)
  listen('menu-close', () => {
    closeMenu();
  });

  // Menu window lost focus (Rust blur handler emitted this)
  listen('menu-closed', () => {
    menuWindowOpen = false;
    if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(false);
    showView('menu-main-view');
  });
}

function flashWindowAttention() {
  // CSS shake is handled in updateStateUI; ask the main process to flash the
  // taskbar / pulse opacity as an additional attention signal.
  if (window.petAPI) {
    window.petAPI.flashAttention();
  }
}

// ===== Context Menu =====
// Whether the separate menu window is currently open. Used by setupContextMenu
// to toggle the menu on ☰ click (the main window's context-menu element is
// always hidden now — the menu lives in its own Tauri window).
let menuWindowOpen = false;
// Menu width (210 CSS) plus a small gap — the horizontal space to grow.
const MENU_SIDE_WIDTH = 300;

/** Bottom edge (logical CSS px) of the status bar — the menu's lower bound. */
function statusBarBottom() {
  const sb = document.getElementById('status-bar');
  return sb ? sb.getBoundingClientRect().bottom : 300;
}

/**
 * Fallback menu side when the backend grow fails: pet on the left half of
 * the screen -> open rightward, and vice versa. Normally the backend picks
 * the side itself (calculate_menu_space) using exact monitor geometry.
 */
function fallbackMenuSide() {
  const winCenterX = (window.screenX ?? 0) + window.innerWidth / 2;
  const screenCenterX = (window.screen.availLeft || 0) + (window.screen.availWidth || window.screen.width) / 2;
  return winCenterX <= screenCenterX ? 'right' : 'left';
}

/**
 * Open the context menu in a SEPARATE Tauri window. The pet window never
 * resizes — the menu window is shown beside it (left or right depending on
 * which half of the monitor the pet is on). Because the pet window's geometry
 * is unchanged, there's zero WebView2 layout lag → zero flicker on BOTH
 * sides. The menu window loads the same index.html with __MENU_MODE__=true
 * and communicates with the main window via Tauri events.
 */
async function positionMenu() {
  // Determine which side of the monitor the pet is on.
  let side = 'right';
  if (window.petAPI && window.petAPI.calculateMenuSpace) {
    try {
      const info = await window.petAPI.calculateMenuSpace(MENU_SIDE_WIDTH);
      side = info.side;
    } catch (e) {
      side = fallbackMenuSide();
    }
  } else {
    side = fallbackMenuSide();
  }

  // Calculate the menu window position in screen coordinates (CSS px).
  const petX = window.screenLeft ?? window.screenX;
  const petY = window.screenTop ?? window.screenY;
  const petW = window.innerWidth;
  const petH = window.innerHeight;
  const menuW = 300;
  // Start taller than the pet so taller views (character grid with 5+ cards)
  // aren't clipped before fitMenuWindow() fine-tunes to the exact content
  // height. Capped to available screen space below the pet's top edge.
  const screenTop = petY ?? 0;
  const maxScreenH = (window.screen.availHeight || 720) - screenTop - 8;
  const menuH = Math.min(Math.max(petH + 160, 500), maxScreenH);
  const menuX = side === 'right'
    ? petX + petW + 4
    : petX - menuW - 4;

  if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(true);

  // Show the menu window at the calculated position.
  if (window.petAPI && window.petAPI.showMenuWindow) {
    try {
      await window.petAPI.showMenuWindow(menuX, petY, menuW, menuH);
      menuWindowOpen = true;
    } catch (e) {
      console.warn('[menu] Failed to show menu window:', e && e.message);
    }
  }

  // Send live data to the menu window so it can populate dynamic items.
  const { emit } = window.__TAURI__.event;
  emit('menu-data', {
    motions: currentMotionGroups,
    flipHorizontal: flipHorizontal,
    miniMode: miniMode,
    hooksInstalled: hooksInstalled,
    hookStatusHint: (document.getElementById('hook-status-hint') || {}).textContent || '',
    currentState: currentState,
    sessions: (currentSnapshot && currentSnapshot.sessions) || [],
  });
}

/**
 * Re-clamp the menu after the content changed (project list update while the
 * menu is open). No-op for the separate menu window approach — the menu
 * window handles its own sizing.
 */
function repositionMenu() {
  // No-op: the menu window is sized by show_menu_window and doesn't need
  // re-clamping when the main window's content changes.
}

/**
 * Resize the menu window to fit the currently-visible view's content height.
 * Called after view switches (showView) and after dynamic content loads
 * (loadCharacters). The menu window is otherwise a fixed size set by
 * positionMenu; without this, taller views (e.g. the character grid with 5+
 * cards) clip the bottom card. Caps at the available screen height below the
 * window's top edge; if content is taller, #context-menu's overflow-y:auto
 * handles scrolling.
 */
function fitMenuWindow() {
  if (!window.__MENU_MODE__) return;
  const cm = document.getElementById('context-menu');
  if (!cm) return;
  // Temporarily lift max-height so offsetHeight reflects the TRUE content
  // height. With max-height:100vh still applied, scrollHeight/offsetHeight
  // can return the clamped height (not the full content) on WebView2, leaving
  // the bottom card clipped. This synchronous toggle causes no visual flash
  // (the browser doesn't paint between style change + offsetHeight read +
  // style restore).
  const prevMaxHeight = cm.style.maxHeight;
  cm.style.maxHeight = 'none';
  const contentH = cm.offsetHeight;
  cm.style.maxHeight = prevMaxHeight;
  // Cap to available screen space below the window's top edge so the menu
  // never extends past the bottom of the screen.
  const screenTop = window.screenTop ?? window.screenY ?? 0;
  const availH = window.screen.availHeight || 720;
  const maxH = availH - screenTop - 8;
  const h = Math.max(200, Math.min(contentH + 4, maxH));
  if (window.petAPI && window.petAPI.resizeMenuWindow) {
    window.petAPI.resizeMenuWindow(300, h);
  }
}

/**
 * Show one of the three menu views (main / model / motion), hiding the others.
 */
function showView(viewId) {
  for (const id of ['menu-main-view', 'menu-model-view', 'menu-motion-view', 'menu-settings-view', 'menu-language-view', 'menu-char-edit-view']) {
    document.getElementById(id).classList.toggle('hidden', id !== viewId);
  }
  // Reset the inline character-creation input whenever switching views.
  // Without this, the input box stays visible after the user clicks
  // "+ 新建形象" and then navigates away and back to the model view.
  const charInline = document.getElementById('char-create-inline');
  if (charInline) charInline.classList.add('hidden');
  // Leaving the motion list ends the hover preview loop.
  if (viewId !== 'menu-motion-view') stopMotionPreview();
  // Fit the menu window height to the newly-visible view's content.
  fitMenuWindow();
}

/** Hide the menu window and reset menu state. */
function closeMenu() {
  if (!menuWindowOpen) return;
  menuWindowOpen = false;
  if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(false);
  showView('menu-main-view');
  if (window.petAPI && window.petAPI.hideMenuWindow) {
    window.petAPI.hideMenuWindow().catch(() => {});
  }
}

function setupContextMenu() {
  const menuBtn = document.getElementById('menu-btn');

  // Toggle open/close on ☰ click. Clicking the button doesn't bubble to the
  // document (stopPropagation), so without a toggle a second click would just
  // reposition the already-open menu.
  menuBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    if (menuWindowOpen) {
      closeMenu();
      return;
    }
    showView('menu-main-view'); // always open on the main view
    positionMenu();
  });

  // Right-click on the pet (model or status bar) opens the same menu.
  document.addEventListener('contextmenu', (e) => {
    // Always suppress the native webview context menu — this window is a pet,
    // not a page.
    e.preventDefault();
    if (menuWindowOpen) {
      closeMenu();
      return;
    }
    const onModel = e.target.closest('#canvas-wrapper') && isPointOnModel(e.clientX, e.clientY);
    const onStatusBar = e.target.closest('#status-bar');
    if (!onModel && !onStatusBar) return; // blank space: no menu
    showView('menu-main-view');
    positionMenu();
  });

  // Clicking anywhere on the pet window closes the menu (the menu window
  // also loses focus, which triggers Rust's blur handler).
  document.addEventListener('click', () => {
    closeMenu();
  });

  // Window losing focus (user clicked another app) also closes the menu.
  // Skip when a file picker is open (set by upload button handler).
  window.addEventListener('blur', () => {
    if (!window.__PICKING_FILE__) closeMenu();
  });

  // Secondary menus: 切换形象 / 播放动作 / 动作设定  ->  pop out the list
  document.getElementById('menu-models-trigger').addEventListener('click', () => {
    // Rescan on every open so freshly uploaded user models appear.
    loadCharacters();
    showView('menu-model-view');
  });

  // 上传 Live2D -> open the user model folder (creates it + README on
  // first use), then the user drops model folders into it.
  document.getElementById('menu-upload-live2d').addEventListener('click', () => {
    closeMenu();
    if (window.petAPI && window.petAPI.openLive2DFolder) {
      Promise.resolve(window.petAPI.openLive2DFolder()).catch((e) =>
        console.warn('[models] open folder failed:', e)
      );
    }
  });

  // 打开声音文件夹 -> open ~/.dutyon/sounds (creates it + README on first
  // use). Users drop {state}.mp3 files here for the external display.
  document.getElementById('menu-sounds-folder').addEventListener('click', () => {
    closeMenu();
    if (window.petAPI && window.petAPI.openSoundsFolder) {
      Promise.resolve(window.petAPI.openSoundsFolder()).catch((e) =>
        console.warn('[sounds] open folder failed:', e)
      );
    }
  });

  document.getElementById('menu-play-motion').addEventListener('click', () => {
    openMotionView('play');
  });

  document.getElementById('menu-settings-trigger').addEventListener('click', () => {
    buildSettingsMenu();
    showView('menu-settings-view');
  });

  // State rows in 动作设定 -> open the motion picker in assign mode.
  document.getElementById('settings-sleeping').addEventListener('click', () => {
    openMotionView('assign', 'sleeping');
  });
  document.getElementById('settings-working').addEventListener('click', () => {
    openMotionView('assign', 'working');
  });
  document.getElementById('settings-alert').addEventListener('click', () => {
    openMotionView('assign', 'alert');
  });

  // 语言 -> open the language picker submenu.
  document.getElementById('menu-language-trigger').addEventListener('click', () => {
    showView('menu-language-view');
  });

  document.getElementById('menu-language-back').addEventListener('click', () => {
    showView('menu-main-view');
  });

  // 开机自启动 -> toggle the Windows Run registry entry.
  document.getElementById('menu-auto-launch').addEventListener('click', () => {
    toggleAutoLaunch();
  });

  // 允许外部访问 -> toggle 0.0.0.0 binding for the /api/* read-only routes
  // (external display). Needs an app restart to take effect.
  document.getElementById('menu-external-access').addEventListener('click', () => {
    toggleExternalAccess();
  });

  // 预览提醒效果 -> trigger a fake confirmation-needed session for 8s.
  document.getElementById('menu-test-alert').addEventListener('click', () => {
    closeMenu();
    if (window.petAPI && window.petAPI.testAlert) window.petAPI.testAlert();
  });

  // ◀ 返回 -> back to the appropriate parent view (menu stays open)
  document.getElementById('menu-model-back').addEventListener('click', () => {
    showView('menu-main-view');
  });

  document.getElementById('menu-motion-back').addEventListener('click', () => {
    // In assign mode the parent is the 动作设定 view; otherwise the main view.
    showView(motionPickerMode === 'assign' ? 'menu-settings-view' : 'menu-main-view');
  });

  document.getElementById('menu-settings-back').addEventListener('click', () => {
    showView('menu-main-view');
  });

  // 左右翻转 toggle (checkbox) — keep the menu open so the ✓ change is visible.
  document.getElementById('menu-flip').addEventListener('click', () => {
    toggleFlip();
  });

  // 迷你模式 toggle (checkbox) — keep the menu open so the ✓ change is visible.
  document.getElementById('menu-mini-mode').addEventListener('click', () => {
    toggleMiniMode();
  });

  // Menu actions
  document.getElementById('menu-install-hooks').addEventListener('click', async () => {
    closeMenu();
    if (window.petAPI) {
      // Always re-run the installer: it is idempotent (dedupes hook entries)
      // and refreshes the bridge script from the bundled copy. Only the
      // prompt differs: first install → "enable in IDE"; re-install →
      // "already active".
      const before = await window.petAPI.isHooksInstalled();
      const result = await window.petAPI.installHooks();
      if (result.success) {
        showInstallResult(true, result, !!(before && before.installed));
        checkHooksStatus(); // refresh the hint after install
      } else {
        showInstallResult(false, result);
      }
    }
  });

  document.getElementById('menu-hook-status').addEventListener('click', async () => {
    closeMenu();
    if (!window.petAPI) return;
    const status = await window.petAPI.isHooksInstalled();
    showHookStatusDialog(status);
  });

  document.getElementById('menu-quit').addEventListener('click', () => {
    if (window.petAPI) {
      window.petAPI.quit();
    }
  });
}

function showInstallResult(success, result, alreadyInstalled) {
  const stateText = document.getElementById('pet-state-text');
  if (success) {
    if (alreadyInstalled) {
      stateText.textContent = i18n.t('hook.alreadyActive');
      stateText.title = i18n.t('hook.alreadyActive');
    } else {
      stateText.textContent = i18n.t('hook.installSuccess');
      stateText.title = [
        i18n.t('hook.installSuccess'),
        '',
        'Trae IDE: Settings -> Hooks -> Local auto-run -> Enable -> New AI session',
        'Qoder: restart the IDE (hooks load automatically on startup)',
        'Cursor: hooks.json reloads on save (restart the IDE if nothing happens)',
        'Codex: run /hooks in the CLI to trust the hook, then start a new session',
        'OpenCode: restart OpenCode to load the bridge plugin (auto-loaded from ~/.config/opencode/plugins/)',
      ].join('\n');
    }
    // Recovery notices (e.g. a foreign/corrupt config was backed up and
    // rebuilt) — surface them in the hover tooltip so users aren't surprised.
    if (result && result.warning) {
      stateText.title += '\n\n⚠ ' + result.warning;
      if (window.__petSendLog) window.__petSendLog('warn', '[hooks] install warning: ' + result.warning);
    }
    // Restore the state label after a few seconds — without this the text
    // sticks forever while the state stays unchanged (setPetState early-
    // returns on same-state, so updateStateUI never re-runs).
    setTimeout(() => { updateStateUI(currentState); }, 6000);
  } else {
    // Show the concrete reason (file path + OS error) in the hover tooltip;
    // the short label alone gave users no way to diagnose the failure.
    const detail = (result && result.error) ? String(result.error) : '';
    stateText.textContent = i18n.t('hook.installFailed');
    stateText.title = detail ? (i18n.t('hook.installFailed') + '\n' + detail) : i18n.t('hook.installFailed');
    if (window.__petSendLog) window.__petSendLog('error', '[hooks] install FAILED: ' + detail);
    setTimeout(() => { updateStateUI(currentState); }, 4000);
  }
}

async function checkHooksStatus() {
  if (!window.petAPI) return;
  const status = await window.petAPI.isHooksInstalled();
  hooksInstalled = !!status.installed;
  // Only refresh the menu hint — never override the status-bar text. The
  // status bar shows the pet state (idle/working/...); install/enable
  // guidance lives in the ☰ menu's Hook 状态 row and dialog.
  updateHookStatusHint(hooksInstalled, currentSnapshot);
}

/**
 * Update the Hook 状态 hint in the menu with install state + last event time.
 */
function updateHookStatusHint(installed, snapshot) {
  const hint = document.getElementById('hook-status-hint');
  if (!hint) return;
  const parts = [];
  if (!installed) {
    parts.push(i18n.t('hook.notInstalled'));
  } else if (snapshot && snapshot.lastEventAt) {
    const ago = Math.max(0, Math.floor((Date.now() - snapshot.lastEventAt) / 1000));
    parts.push(i18n.t('hook.connected'));
    parts.push(ago < 60 ? i18n.t('hook.secondsAgo', { n: ago }) : i18n.t('hook.minutesAgo', { n: Math.floor(ago / 60) }));
  } else {
    parts.push(i18n.t('hook.needsEnable'));
  }
  hint.textContent = parts.join(' · ');
}

/**
 * Show a transient dialog with detailed Hook diagnostic info.
 */
function showHookStatusDialog(status) {
  const stateText = document.getElementById('pet-state-text');
  const original = stateText.textContent;
  const lines = [
    `${i18n.t('hook.dialogConfig')}: ${status.installed ? i18n.t('hook.installed') : i18n.t('hook.notInstalledLabel')}`,
    `${i18n.t('hook.dialogBridge')}: ${status.bridgeExists ? i18n.t('hook.exists') : i18n.t('hook.missing')}`,
    `${i18n.t('hook.dialogHooksJson')}: ${status.hooksExist ? i18n.t('hook.exists') : i18n.t('hook.missing')}`,
    `${i18n.t('hook.dialogQoderSettings')}: ${status.qoderHooksExist ? i18n.t('hook.exists') : i18n.t('hook.missing')}`,
    `${i18n.t('hook.dialogCursorHooks')}: ${status.cursorHooksExist ? i18n.t('hook.exists') : i18n.t('hook.missing')}`,
    `${i18n.t('hook.dialogCodexHooks')}: ${status.codexHooksExist ? i18n.t('hook.exists') : i18n.t('hook.missing')}`,
    `${i18n.t('hook.dialogOpencodePlugin')}: ${status.opencodePluginExist ? i18n.t('hook.exists') : i18n.t('hook.missing')}`,
  ];
  stateText.textContent = status.installed ? i18n.t('hook.dialogInstalledMsg') : i18n.t('hook.dialogNotInstalledMsg');
  stateText.title = lines.join('\n') + '\n\n' + i18n.t('hook.dialogHint');
  setTimeout(() => {
    stateText.textContent = original;
    stateText.title = '';
  }, 4000);
}

// ===== Edge Dock (screen-edge snap) =====
// Dragging the window within the snap threshold of the monitor's LEFT or
// RIGHT edge docks it as a compact "traffic-light" bar (content-sized height,
// not full screen edge): Live2D + status bar are hidden, each project is
// shown as a two-letter badge colored by its session status, and dragging the
// bar away from the edge restores the full window.

/**
 * Lazily create the dock bar overlay (hidden until body.edge-dock is set).
 */
function ensureEdgeDockBar() {
  if (edgeDockEl) return edgeDockEl;
  const bar = document.createElement('div');
  bar.id = 'edge-dock-bar';
  // Round status lamp at the top; its color tracks the most urgent session
  // status (see updateEdgeDockIndicator).
  const indicator = document.createElement('div');
  indicator.id = 'edge-dock-indicator';
  indicator.className = 'level-idle';
  const badges = document.createElement('div');
  badges.id = 'edge-dock-badges';
  bar.appendChild(indicator);
  bar.appendChild(badges);
  document.body.appendChild(bar);
  // Click a badge to bring its IDE window to front (same as the project list
  // rows); suppressed right after a drag so undocking never triggers it.
  bar.addEventListener('click', (e) => {
    if (Date.now() - edgeDockDragEndAt < 250) return;
    const badge = e.target.closest('.edge-dock-badge');
    if (!badge || !badge.dataset.target || !window.petAPI) return;
    window.petAPI.bringToFront(badge.dataset.target);
  });
  // Double-click empty bar space also restores the full window.
  bar.addEventListener('dblclick', (e) => {
    if (e.target.closest('.edge-dock-badge')) return;
    leaveEdgeDock();
  });
  edgeDockEl = bar;
  return bar;
}

/**
 * Two-uppercase-letter abbreviation for a project name: initials of the first
 * two words when available ("my app" -> "MA"), else the first two characters
 * ("myapp" -> "MY").
 */
function projectInitials(name) {
  const trimmed = String(name || '').trim();
  const chars = [...trimmed];
  if (chars.length === 0) return '--';
  const words = trimmed.split(/[\s\-_./\\]+/).filter(Boolean);
  if (words.length >= 2) return ([...words[0]][0] + [...words[1]][0]).toUpperCase();
  return chars.slice(0, 2).join('').toUpperCase();
}

function dockStatusText(status) {
  if (status === 'confirmation-needed') return i18n.t('status.confirmationNeeded');
  if (status === 'thinking') return i18n.t('status.thinking');
  if (status === 'tool-use') return i18n.t('status.toolUse');
  if (status === 'working') return i18n.t('status.busy');
  return i18n.t('status.idle');
}

/**
 * Rebuild the two-letter project badges from the latest snapshot. Called
 * alongside updateStatusBar so badge colors track session status live.
 */
function renderEdgeDockBadges() {
  if (!edgeDockEl) return;
  const holder = edgeDockEl.querySelector('#edge-dock-badges');
  if (!holder) return;
  updateEdgeDockIndicator();
  holder.innerHTML = '';
  const sessions = (currentSnapshot && currentSnapshot.sessions) || [];
  if (sessions.length === 0) {
    const badge = document.createElement('div');
    badge.className = 'edge-dock-badge empty';
    badge.textContent = '··';
    holder.appendChild(badge);
    return;
  }
  for (const session of sessions) {
    const badge = document.createElement('div');
    badge.className = `edge-dock-badge status-${session.status}`;
    badge.textContent = projectInitials(session.projectName);
    badge.title = `${session.projectName} — ${dockStatusText(session.status)}`;
    // Window-detected sessions have no projectPath; fall back to projectName
    // (bring-to-front matches by folder name in the window title).
    badge.dataset.target = session.projectPath || session.projectName || '';
    holder.appendChild(badge);
  }
}

/**
 * Top-of-bar round status lamp: reflects the most urgent session status —
 * red (confirmation-needed) > yellow (working) > blue (idle), matching the
 * project-dot palette in styles.css.
 */
function updateEdgeDockIndicator() {
  if (!edgeDockEl) return;
  const indicator = edgeDockEl.querySelector('#edge-dock-indicator');
  if (!indicator) return;
  const sessions = (currentSnapshot && currentSnapshot.sessions) || [];
  let level = 'idle';
  for (const s of sessions) {
    if (s.status === 'confirmation-needed') {
      level = 'alert';
      break;
    }
    // All active variants (working/thinking/tool-use) light the working lamp.
    if (s.status === 'working' || s.status === 'thinking'
        || s.status === 'tool-use') {
      level = 'working';
    }
  }
  indicator.className = `level-${level}`;
  indicator.title = level === 'alert'
    ? i18n.t('status.confirmationNeeded')
    : level === 'working' ? i18n.t('status.busy') : i18n.t('status.idle');
}

/**
 * Desired dock bar height (logical CSS px) based on the badge count — the
 * bar hugs its content (indicator + badges) instead of spanning the whole
 * screen edge. Must match the CSS metrics in styles.css.
 */
function computeDockBarHeight() {
  const n = Math.max(1, ((currentSnapshot && currentSnapshot.sessions) || []).length);
  const PAD = 8, INDICATOR = 22, GAP = 8, BADGE = 22, BADGE_GAP = 6;
  return PAD * 2 + INDICATOR + GAP + n * BADGE + (n - 1) * BADGE_GAP;
}

/**
 * After a drag ends, ask the backend whether the window landed near the
 * left/right screen edge; if so snap into dock mode (window geometry handled
 * by Rust, UI here).
 */
async function maybeEnterEdgeDock() {
  if (!window.petAPI || !window.petAPI.detectEdgeDock) return;
  // Never dock with the menu still open (it would be hidden by the dock bar
  // and leave the window grown).
  closeMenu();
  try {
    const edge = await window.petAPI.detectEdgeDock();
    if (!edge || edgeDocked || isDragging) return;
    await window.petAPI.enterEdgeDock(edge, computeDockBarHeight());
    edgeDocked = true;
    document.body.classList.add('edge-dock', `edge-dock-${edge}`);
    ensureEdgeDockBar();
    renderEdgeDockBadges();
  } catch (err) {
    console.warn('[edgeDock] enter failed:', err && err.message);
  }
}

/**
 * Restore the full window (size + layout). Called mid-drag when the docked
 * bar is pulled away from the edge, and on double-click. The backend restores
 * the pre-dock size and pulls the window off the edge so the drag continues
 * seamlessly and an immediate re-snap is avoided.
 */
function leaveEdgeDock() {
  if (!edgeDocked) return;
  edgeDocked = false;
  document.body.classList.remove('edge-dock', 'edge-dock-left', 'edge-dock-right');
  if (window.petAPI && window.petAPI.exitEdgeDock) {
    Promise.resolve(window.petAPI.exitEdgeDock()).catch((e) => {
      console.warn('[edgeDock] exit failed:', e && e.message);
    });
  }
}

// ===== Drag Setup =====
function setupDrag() {
  const wrapper = document.getElementById('canvas-wrapper');
  let lastX = 0;
  let lastY = 0;
  let hasMoved = false;

  // Shared drag start used by the model canvas, the status bar, and the
  // edge-dock bar.
  function beginDrag(e) {
    isDragging = true;
    // Keep the window clickable for the whole drag — the cursor may leave the
    // model bounds mid-drag, which would otherwise flip the window to
    // click-through (via the Rust polling loop) and break dragging.
    if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(true);
    // Drop any stale snap-preview ghost from an interrupted previous drag.
    if (window.petAPI && window.petAPI.hideDockPreview) window.petAPI.hideDockPreview();
    hasMoved = false;
    lastX = e.screenX;
    lastY = e.screenY;
  }

  wrapper.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return; // Only left click
    if (edgeDocked) return;
    // Only start dragging when the pointer is actually over the model's bounds,
    // not on the empty canvas padding — matches the click-through hit area so
    // the model interior is both the tap target and the drag target.
    if (!isPointOnModel(e.clientX, e.clientY)) return;
    beginDrag(e);
  });

  // Edge-dock bar: the whole bar is the drag handle. Dragging it away from
  // the edge restores the full window mid-drag (see mousemove below).
  const dockBar = ensureEdgeDockBar();
  dockBar.addEventListener('mousedown', (e) => {
    if (e.button !== 0 || !edgeDocked) return;
    beginDrag(e);
  });

  let dragPendingX = 0, dragPendingY = 0, dragRafId = null;
  let lastPreviewAt = 0;
  document.addEventListener('mousemove', (e) => {
    if (!isDragging) return;
    const deltaX = e.screenX - lastX;
    const deltaY = e.screenY - lastY;
    if (Math.abs(deltaX) > 1 || Math.abs(deltaY) > 1) {
      hasMoved = true;
    }
    // Dragging the docked bar restores the full window and continues the drag
    // seamlessly.
    if (hasMoved && edgeDocked) leaveEdgeDock();
    lastX = e.screenX;
    lastY = e.screenY;
    if (hasMoved && window.petAPI) {
      // Coalesce multiple mousemove deltas into a single rAF IPC call so a
      // 60Hz mouse doesn't fire 30+ drag_window invocations per second.
      dragPendingX += deltaX;
      dragPendingY += deltaY;
      if (dragRafId === null) {
        dragRafId = requestAnimationFrame(() => {
          const moved = window.petAPI.dragWindow(dragPendingX, dragPendingY);
          dragPendingX = 0;
          dragPendingY = 0;
          dragRafId = null;
          // Ghost preview of the upcoming edge snap. Updated AFTER the window
          // actually moved (so the geometry matches), and throttled to ~20Hz
          // so a fast drag doesn't flood the preview window with moves.
          if (!edgeDocked && window.petAPI.updateDockPreview) {
            const now = performance.now();
            if (now - lastPreviewAt >= 50) {
              lastPreviewAt = now;
              Promise.resolve(moved)
                .then(() => window.petAPI.updateDockPreview(computeDockBarHeight()))
                .catch(() => { /* preview is cosmetic — never block the drag */ });
            }
          }
        });
      }
    }
  });

  document.addEventListener('mouseup', async () => {
    const dragged = isDragging && hasMoved;
    isDragging = false;
    if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(false);
    if (!dragged) return;
    edgeDockDragEndAt = Date.now();
    // Flush any drag delta still queued in rAF and wait for it to apply, so
    // snap detection sees the window's FINAL position. Skipping this made the
    // edge threshold misfire: the last frame of movement (up to ~30px on a
    // fast drag) was still pending when detect_edge_dock measured the window.
    if (dragRafId !== null) {
      cancelAnimationFrame(dragRafId);
      dragRafId = null;
    }
    if ((dragPendingX !== 0 || dragPendingY !== 0) && window.petAPI && window.petAPI.dragWindow) {
      const fx = dragPendingX, fy = dragPendingY;
      dragPendingX = 0;
      dragPendingY = 0;
      try { await window.petAPI.dragWindow(fx, fy); } catch (e) { /* keep going */ }
    }
    // Drag over — the ghost has served its purpose. Either the snap below
    // replaces it with the real dock bar, or no snap happens and it must go.
    if (window.petAPI && window.petAPI.hideDockPreview) {
      try { await window.petAPI.hideDockPreview(); } catch (e) { /* cosmetic */ }
    }
    // After a real drag ends, snap to the nearest left/right screen edge when
    // we landed within the threshold.
    if (!edgeDocked) maybeEnterEdgeDock();
  });

  // Also support drag on the status bar
  const statusBar = document.getElementById('status-bar');
  statusBar.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    if (edgeDocked) return;
    if (e.target.closest('.project-item') || e.target.closest('#menu-btn')) return;
    beginDrag(e);
  });
}

// ===== Click-Through =====
/**
 * Test whether a screen point (clientX/Y) is over the Live2D model's hit area.
 * Uses the model's current world-space bounding box (getBounds()); empty
 * canvas padding outside the box returns false and passes clicks through to
 * the desktop.
 */
function isPointOnModel(clientX, clientY) {
  // No Live2D model (custom animation / GIF mode): check canvas-wrapper bounds.
  if (!live2dModel || !pixiApp) {
    const wrapper = document.getElementById('canvas-wrapper');
    if (!wrapper) return false;
    const rect = wrapper.getBoundingClientRect();
    return clientX >= rect.left && clientX <= rect.right &&
           clientY >= rect.top && clientY <= rect.bottom;
  }
  const canvas = pixiApp.view;
  const rect = canvas.getBoundingClientRect();
  const x = clientX - rect.left;
  const y = clientY - rect.top;
  if (x < 0 || y < 0 || x > rect.width || y > rect.height) return false;
  try {
    const b = live2dModel.getBounds();
    return x >= b.x && x <= b.x + b.width && y >= b.y && y <= b.y + b.height;
  } catch (e) {
    return false;
  }
}

/**
 * Dynamic click-through: let transparent (empty) areas of the window pass mouse
 * events through to the desktop, while keeping the model, status bar, and menu
 * interactive.
 *
 * Tauri's set_ignore_cursor_events has NO {forward:true} mode — once the
 * renderer sets ignore=true it stops receiving mousemove entirely and can
 * never toggle back. So click-through is driven from the Rust side: a polling
 * thread reads Win32 GetCursorPos and toggles set_ignore_cursor_events based
 * on "clickable rectangles" we report here. The PixiJS ticker (rAF) keeps
 * running while click-through, so we keep reporting regions even with no mouse
 * input. Drag and menu-open use setForceClickable(true) as a belt-and-suspenders
 * override (see setupDrag / positionMenu / closeMenu).
 *
 * Must be called after initPixiApp() (it registers a ticker callback on pixiApp).
 */
function setupClickThrough() {
  if (setupClickThrough._done) return;
  setupClickThrough._done = true;
  if (!window.petAPI || !window.petAPI.updateClickRegions) return;

  // Convert a CSS-px DOMRect to physical-pixel window-local coords for Rust.
  // devicePixelRatio is read fresh each call so a monitor/DPI change mid-run
  // re-scales correctly.
  const toPhys = (r) => {
    const dpr = window.devicePixelRatio || 1;
    return {
      x: Math.round(r.left * dpr),
      y: Math.round(r.top * dpr),
      width: Math.round(r.width * dpr),
      height: Math.round(r.height * dpr),
    };
  };

  // Collect clickable rectangles: model bounds + status bar + (visible) menu.
  function collectRegions() {
    const rects = [];
    if (edgeDocked) {
      // The docked bar fills the entire thin window — keep it all clickable.
      rects.push(toPhys({ left: 0, top: 0, width: window.innerWidth, height: window.innerHeight }));
      return rects;
    }
    // Model bounds are canvas-local px; offset by the canvas's window-local
    // position to get window-local coords, then scale to physical px.
    if (live2dModel && pixiApp) {
      try {
        const canvasRect = pixiApp.view.getBoundingClientRect();
        const b = live2dModel.getBounds();
        const dpr = window.devicePixelRatio || 1;
        rects.push({
          x: Math.round((canvasRect.left + b.x) * dpr),
          y: Math.round((canvasRect.top + b.y) * dpr),
          width: Math.round(b.width * dpr),
          height: Math.round(b.height * dpr),
        });
      } catch (e) { /* model not ready yet */ }
    } else {
      // No Live2D model (custom animation / GIF mode): use the canvas wrapper
      // as the clickable region so the pet remains interactive.
      const wrapper = document.getElementById('canvas-wrapper');
      if (wrapper) rects.push(toPhys(wrapper.getBoundingClientRect()));
    }
    const sb = document.getElementById('status-bar');
    if (sb) rects.push(toPhys(sb.getBoundingClientRect()));
    const cm = document.getElementById('context-menu');
    if (cm && !cm.classList.contains('hidden')) rects.push(toPhys(cm.getBoundingClientRect()));
    return rects;
  }

  // Report regions periodically. Uses PixiJS ticker if available (Live2D mode),
  // otherwise falls back to setInterval (GIF/MP4 mode where pixiApp is null).
  // Throttle to ~8Hz: getBounds() is relatively expensive and CSS rects barely move.
  // Dirty check: only send the IPC when the JSON of regions changed since the
  // last report, so idle frames don't spam update_click_regions.
  let lastRegionsJson = '';
  const reportRegions = () => {
    const regions = collectRegions();
    const json = JSON.stringify(regions);
    if (json !== lastRegionsJson) {
      lastRegionsJson = json;
      window.petAPI.updateClickRegions(regions);
    }
  };

  if (pixiApp && pixiApp.ticker) {
    let frameSkip = 0;
    pixiApp.ticker.add(() => {
      frameSkip = (frameSkip + 1) % 3;
      if (frameSkip !== 0) return;
      reportRegions();
    });
  } else {
    // No pixiApp (GIF/MP4 mode): use setInterval instead of PixiJS ticker.
    setInterval(reportRegions, 120);
  }

  // Seed regions immediately so the Rust polling thread (30ms) has data before
  // the first ticker fire (~125ms) — otherwise transparent areas would block
  // the desktop briefly at startup.
  const seedRegions = collectRegions();
  lastRegionsJson = JSON.stringify(seedRegions);
  window.petAPI.updateClickRegions(seedRegions);
}

// ===== Start =====
init();
