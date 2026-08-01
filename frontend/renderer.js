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
let availableModels = [];      // catalog from main process
let currentModelUrl = null;    // persisted/loaded model URL
let motionPickerMode = 'play'; // 'play' = tap-to-play, 'assign' = pick for a state
let motionPickerTarget = null; // target state when motionPickerMode === 'assign'
let flipHorizontal = false;   // mirror the model horizontally (for left-side placement)
let headEffectEl = null;        // #head-effect container, positioned at head-top anchor
let isDragging = false;         // shared with setupDrag so click-through can stay off while dragging

// Model URLs to try (in order)
const MODEL_URLS = [
  'assets/live2d/nito.model3.json',
  'https://cdn.jsdelivr.net/gh/guansss/pixi-live2d-display/test/assets/hiyori/hiyori_pro_t10.model3.json',
];

// ===== Constants =====
const PIXI_WIDTH = 240;
const PIXI_HEIGHT = 260;
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
  loadAutoLaunchState();
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

// ===== Update check =====
let updateDownloaded = false;

/**
 * Trigger an update check via the main process (electron-updater).
 * Status updates arrive asynchronously via onUpdateStatus.
 */
function checkForUpdates() {
  if (!window.petAPI || !window.petAPI.checkForUpdates) return;
  showUpdateStatus(i18n.t('update.checking'));
  window.petAPI.checkForUpdates();
}

/**
 * Display an update status message in the pet state text area.
 * Cleared after 5 seconds (or when a new state-update arrives).
 */
function showUpdateStatus(message) {
  const stateText = document.getElementById('pet-state-text');
  if (!stateText) return;
  stateText.textContent = message;
}

/**
 * Handle update status events from the main process.
 */
function handleUpdateStatus(data) {
  switch (data.status) {
    case 'checking':
      showUpdateStatus(i18n.t('update.checking'));
      break;
    case 'available':
      showUpdateStatus(i18n.t('update.available', { version: data.version }));
      break;
    case 'not-available':
      showUpdateStatus(i18n.t('update.notAvailable'));
      setTimeout(() => updateStateUI(currentState), 4000);
      break;
    case 'downloading':
      showUpdateStatus(i18n.t('update.downloading', { percent: data.percent || 0 }));
      break;
    case 'downloaded':
      updateDownloaded = true;
      showUpdateStatus(i18n.t('update.downloaded', { version: data.version }));
      break;
    case 'error':
      if (data.errorType === 'network') {
        showUpdateStatus(i18n.t('update.networkError'));
      } else {
        showUpdateStatus(i18n.t('update.error', { message: data.message || '' }));
      }
      setTimeout(() => updateStateUI(currentState), 5000);
      break;
  }
}

// ===== Initialize =====
async function init() {
  if (window.__petSendLog) window.__petSendLog('info', '[init] start');
  // Load the UI language (persisted or OS default) before any UI text renders.
  await loadLanguage();
  if (window.__petSendLog) window.__petSendLog('info', '[init] language loaded');
  applyTranslations();

  setupIPC();
  setupContextMenu();
  setupDrag();
  // setupClickThrough() is called from initPixiApp() once pixiApp exists (it
  // registers a PixiJS ticker callback).
  buildMotionMenu('play');

  // Fetch available models + persisted choice from the main process, then
  // build the model-switching menu.
  await loadModelCatalog();
  if (window.__petSendLog) window.__petSendLog('info', '[init] model catalog loaded, currentModelUrl=' + currentModelUrl);

  // Load persisted per-state motion assignments before the first
  // playStateMotion() runs in initLive2D().
  await loadStateMotions();
  buildSettingsMenu();

  // Load persisted appearance settings (horizontal flip) before the model is
  // attached, so attachModel applies the flip on first render.
  await loadAppearance();
  updateFlipMenuCheck();

  // Wait for libraries to load
  if (window.__petSendLog) window.__petSendLog('info', '[init] waiting for libs...');
  const libsReady = await waitForLibs();
  if (window.__petSendLog) window.__petSendLog('info', '[init] libsReady=' + libsReady);

  if (libsReady) {
    await initLive2D();
  }

  if (!live2dModel) {
    // Fallback to canvas animation
    if (window.__petSendLog) window.__petSendLog('warn', '[init] no model, using fallback canvas');
    initFallbackCanvas();
    setPetState('sleeping');
  }

  // Ensure the "初始化中..." placeholder is replaced with the real state
  // label. setPetState('sleeping') is a no-op when currentState is already
  // 'sleeping' (its guard clause returns early), so we must call updateStateUI
  // directly to refresh the text + status bar.
  updateStateUI(currentState);

  // Start effects loop
  startEffectsLoop();

  // Check if hooks are installed + show diagnostic status
  checkHooksStatus();

  // Register update status callback (electron-updater events).
  if (window.petAPI && window.petAPI.onUpdateStatus) {
    window.petAPI.onUpdateStatus(handleUpdateStatus);
  }

  // Post-init diagnostic: check rendering state after 3s to diagnose
  // "completely transparent / can't click" issues.
  setTimeout(() => {
    try {
      const canvasEl = document.querySelector('#live2d-canvas canvas');
      const statusBar = document.getElementById('status-bar');
      const sbStyle = statusBar ? getComputedStyle(statusBar) : null;
      const bodyStyle = getComputedStyle(document.body);
      const diag = [
        'win=' + window.innerWidth + 'x' + window.innerHeight,
        'styleSheets=' + document.styleSheets.length,
        'body.bg=' + bodyStyle.backgroundColor,
        'body.display=' + bodyStyle.display,
        'sb.display=' + (sbStyle ? sbStyle.display : 'null'),
        'sb.bg=' + (sbStyle ? sbStyle.backgroundColor : 'null'),
        'sb.vis=' + (sbStyle ? sbStyle.visibility : 'null'),
        'sb.opacity=' + (sbStyle ? sbStyle.opacity : 'null'),
        'canvas=' + (canvasEl ? canvasEl.width + 'x' + canvasEl.height : 'NOT FOUND'),
        'canvas.parent=' + (canvasEl ? (canvasEl.parentElement ? canvasEl.parentElement.id : 'no parent') : 'N/A'),
        'stage.children=' + (pixiApp ? pixiApp.stage.children.length : 'no pixiApp'),
        'model=' + (live2dModel ? 'exists' : 'null'),
      ];
      if (live2dModel) {
        try {
          const b = live2dModel.getBounds();
          diag.push('model.bounds=' + b.x + ',' + b.y + ',' + b.width + 'x' + b.height);
          diag.push('model.scale=' + live2dModel.scale.x + ',' + live2dModel.scale.y);
          diag.push('model.pos=' + live2dModel.x + ',' + live2dModel.y);
          diag.push('model.visible=' + live2dModel.visible);
          diag.push('model.alpha=' + live2dModel.alpha);
        } catch (e) { diag.push('model.err=' + e.message); }
      }
      if (pixiApp) {
        diag.push('pixi.running=' + pixiApp.ticker.started);
        diag.push('pixi.FPS=' + Math.round(pixiApp.ticker.FPS));
      }
      diag.push('visState=' + document.visibilityState);
      diag.push('hasFocus=' + document.hasFocus());
      if (window.__petSendLog) window.__petSendLog('info', '[diag] ' + diag.join(' | '));
    } catch (e) {
      if (window.__petSendLog) window.__petSendLog('error', '[diag] error: ' + e.message);
    }
  }, 3000);
}

/**
 * Fetch the model catalog and persisted choice from the main process, then
 * populate the "切换形象" menu.
 */
async function loadModelCatalog() {
  if (!window.petAPI || !window.petAPI.getModels) return;
  try {
    const { models, currentModelUrl: saved } = await window.petAPI.getModels();
    availableModels = Array.isArray(models) ? models : [];
    // Normalize old Electron paths (../../assets/...) to Tauri's flat (assets/...)
    // The persisted config from the Electron era uses ../../ prefix which is
    // wrong under Tauri's http://tauri.localhost/ origin.
    let url = saved || (availableModels[0] && availableModels[0].url) || null;
    if (url && url.startsWith('../../')) {
      url = url.replace(/^(\.\.\/)+/, '');
      if (window.__petSendLog) window.__petSendLog('info', '[models] normalized URL: ' + saved + ' → ' + url);
      // Persist the corrected URL so it doesn't re-trigger every launch.
      if (window.petAPI.switchModel) window.petAPI.switchModel(url);
    }
    currentModelUrl = url;
    buildModelMenu();
  } catch (err) {
    console.warn('[models] Failed to load catalog:', err.message);
  }
}

/**
 * Build the model-switching menu items from `availableModels`.
 */
function buildModelMenu() {
  const container = document.getElementById('model-list');
  if (!container) return;
  container.innerHTML = '';
  for (const model of availableModels) {
    const item = document.createElement('div');
    item.className = 'menu-item';
    item.dataset.url = model.url;
    item.textContent = model.name;
    if (model.url === currentModelUrl) item.classList.add('active');
    item.addEventListener('click', () => {
      if (model.url === currentModelUrl) return;
      switchModel(model.url);
      closeMenu();
    });
    container.appendChild(item);
  }
}

/**
 * Highlight the active model in the menu.
 */
function updateModelMenuActive(url) {
  const items = document.querySelectorAll('#model-list .menu-item');
  items.forEach((el) => {
    el.classList.toggle('active', el.dataset.url === url);
  });
}

/**
 * Wait for PixiJS and Live2D libraries to be loaded.
 */
function waitForLibs() {
  return new Promise((resolve) => {
    window.addEventListener('libs-ready', () => {
      if (window.__petSendLog) window.__petSendLog('info', '[waitForLibs] libs-ready event');
      resolve(true);
    }, { once: true });
    window.addEventListener('libs-failed', () => {
      if (window.__petSendLog) window.__petSendLog('warn', '[waitForLibs] libs-failed event');
      resolve(false);
    }, { once: true });

    // If already loaded (script finished before listener attached)
    if (window.PIXI && window.PIXI.live2d) {
      if (window.__petSendLog) window.__petSendLog('info', '[waitForLibs] already loaded');
      resolve(true);
    }
  });
}

// ===== Live2D Initialization =====
/**
 * Create the PixiJS application (once) and load the initial model.
 * The model URL order is: persisted choice → built-in MODEL_URLS fallbacks.
 */
async function initLive2D() {
  try {
    if (window.__petSendLog) window.__petSendLog('info', '[live2d] init start');
    initPixiApp();
    if (window.__petSendLog) window.__petSendLog('info', '[live2d] pixi app created');
    await loadInitialModel();
    if (window.__petSendLog) window.__petSendLog('info', '[live2d] init done, model=' + (live2dModel ? 'loaded' : 'null'));
  } catch (err) {
    if (window.__petSendLog) window.__petSendLog('error', '[live2d] init FAILED: ' + (err && err.message ? err.message : err));
    console.error('[Live2D] Initialization failed:', err);
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
    width: PIXI_WIDTH,
    height: PIXI_HEIGHT,
    backgroundAlpha: 0,
    antialias: true,
    autoStart: true,
    // Prefer the discrete GPU on dual-GPU systems (common on laptops).
    powerPreference: 'high-performance',
  });
  const canvasContainer = document.getElementById('live2d-canvas');
  canvasContainer.appendChild(pixiApp.view);

  // Fixed 24fps cap — smooth enough for Live2D idle animations while keeping
  // CPU/GPU usage low (important over Remote Desktop where every frame adds
  // network bandwidth). No focus/blur switching needed.
  pixiApp.ticker.maxFPS = 24;

  // FPS monitor: log average FPS once per second so we can diagnose lag.
  // Output goes to the main-process console via webContents 'console-message'.
  let fpsFrames = 0;
  let fpsLast = performance.now();
  pixiApp.ticker.add(() => {
    fpsFrames++;
    const now = performance.now();
    if (now - fpsLast >= 1000) {
      const fps = Math.round((fpsFrames * 1000) / (now - fpsLast));
      console.log(`[FPS] ${fps} fps | focused=${document.hasFocus()} | maxFPS=${pixiApp.ticker.maxFPS}`);
      fpsFrames = 0;
      fpsLast = now;
    }
  });

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
      if (window.__petSendLog) window.__petSendLog('info', '[live2d] trying model: ' + url);
      console.log('[Live2D] Trying model:', url);
      live2dModel = await Live2DModel.from(url);
      currentModelUrl = url;
      attachModel(live2dModel);
      // currentState is already 'sleeping' at init, so setPetState would be a
      // no-op — kick off the state motion directly.
      oneShotPlaying = false;
      playStateMotion(MOTION_PRIORITY_FORCE);
      if (window.__petSendLog) window.__petSendLog('info', '[live2d] model loaded OK: ' + url);
      console.log('[Live2D] Model loaded successfully:', url);
      return;
    } catch (err) {
      if (window.__petSendLog) window.__petSendLog('warn', '[live2d] model FAIL ' + url + ': ' + (err && err.message ? err.message : err));
      console.warn('[Live2D] Failed to load from:', url, err.message);
    }
  }
  live2dModel = null;
}

/**
 * Scale, position, and wire up interaction/parameter hooks for a model.
 */
function attachModel(model) {
  const scale = Math.min(
    pixiApp.view.width / model.width,
    pixiApp.view.height / model.height
  ) * 0.72;

  model.scale.set(scale);
  // Mirror horizontally if the user enabled 左右翻转. anchor is (0.5, 1) so
  // flipping scale.x keeps the model centered (no position jump).
  if (flipHorizontal) model.scale.x = -scale;
  model.anchor.set(0.5, 1);
  model.x = pixiApp.view.width / 2;
  model.y = pixiApp.view.height;

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
    if (oneShotPlaying) {
      oneShotPlaying = false;
      // One-shot finished -> restore the head-top effect (ZZZ / dots / !).
      setHeadEffectVisible(true);
    }
    setTimeout(() => {
      if (!oneShotPlaying && !mm.destroyed) playStateMotion();
    }, 0);
  });

  // Tap the pet -> play a different motion once; the state motion resumes when
  // it finishes.
  model.on('pointerdown', () => {
    playRandomOneShot();
  });

  // Set up the head-top effect (ZZZ / working dots / !) for the current state.
  setupHeadEffect();
}

/**
 * Per-frame: track the model's bounds and move #head-effect to the head-top
 * anchor (bounds top-center shifted 40px down). Registered on the PixiJS
 * ticker in setupHeadEffect. getBounds() accounts for scale/anchor/animation,
 * so the effect stays glued to the head as the model breathes/moves.
 */
function updateHeadEffectAnchor() {
  if (!live2dModel || !headEffectEl) return;
  // Throttle: getBounds() computes the full vertex bounding box and is
  // relatively expensive. The CSS head-effect doesn't need pixel-exact
  // per-frame tracking, so update every 3rd frame (~20fps at 60fps ticker).
  updateHeadEffectAnchor._skip = ((updateHeadEffectAnchor._skip || 0) + 1) % 3;
  if (updateHeadEffectAnchor._skip !== 0) return;
  const b = live2dModel.getBounds();
  const topX = b.x + b.width / 2;
  const topY = b.y + 40;
  updateHeadEffectPosition(topX, topY);
}

/**
 * Create (or reset) the #head-effect container and seed it with the current
 * state's effect markup. Called from attachModel once the model is on stage.
 */
function setupHeadEffect() {
  headEffectEl = document.getElementById('head-effect');
  if (!headEffectEl) return;
  updateHeadEffectContent(currentState);
  // Register per-frame anchor tracking so the effect follows the model.
  // Re-register on model switch to avoid stacking listeners.
  const PIXI = window.PIXI;
  if (PIXI && pixiApp) {
    pixiApp.ticker.remove(updateHeadEffectAnchor);
    pixiApp.ticker.add(updateHeadEffectAnchor);
  }
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
 * Move the #head-effect anchor to (x, y) in canvas-wrapper pixel coords.
 * Called every frame from updateDebugBounds so the effect tracks the model.
 */
function updateHeadEffectPosition(x, y) {
  if (!headEffectEl) return;
  headEffectEl.style.transform = `translate(${x}px, ${y}px)`;
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
    console.log('[Live2D] Switching to:', url);
    live2dModel = await Live2DModel.from(url);
    currentModelUrl = url;
    attachModel(live2dModel);
    // State hasn't changed (setPetState would no-op), so replay the state
    // motion on the new model directly.
    oneShotPlaying = false;
    playStateMotion(MOTION_PRIORITY_FORCE);
    updateModelMenuActive(url);
    if (window.petAPI && window.petAPI.switchModel) {
      window.petAPI.switchModel(url);
    }
    console.log('[Live2D] Switched successfully:', url);
  } catch (err) {
    console.error('[Live2D] Switch failed, falling back:', err.message);
    // Restore the previous model if the new one failed to load
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
 */
const STATE_MOTIONS = {
  sleeping: ['Flick3', 1],       // 哈欠
  working:  ['FlickLeft', 1],    // 走路
  alert:    ['FlickLeft', 0],    // yeah
};

/**
 * All available motions for the menu list (group, index, i18nKey).
 * The third element is an i18n key resolved via i18n.t() at display time.
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
 * Play the current state's motion so it loops. `motionFinish` (registered in
 * attachModel) re-triggers this on each finish for seamless looping; this is
 * also called directly on state changes and as a periodic safety net.
 */
function playStateMotion(priority = MOTION_PRIORITY_NORMAL) {
  if (!live2dModel) return;
  const [group, idx] = STATE_MOTIONS[currentState] || STATE_MOTIONS.sleeping;
  try {
    live2dModel.motion(group, idx, priority);
  } catch (e) { /* ignore */ }
}

/**
 * Periodic safety net: if no motion is currently playing (e.g. motionFinish
 * was missed), replay the state motion. Skipped while a one-shot runs.
 */
function triggerPeriodicMotion() {
  if (!live2dModel) return;
  if (oneShotPlaying) return;
  const mm = live2dModel.internalModel.motionManager;
  if (mm.playing) return; // let the current motion finish; motionFinish loops it
  playStateMotion();
}

/**
 * Play a specific motion as a one-shot at FORCE priority (overrides the
 * looping state motion). When it finishes, the motionFinish handler resumes
 * the state motion.
 */
function playMotionOnce(group, idx) {
  if (!live2dModel) return;
  oneShotPlaying = true;
  // Hide the head-top effect while the one-shot plays; motionFinish restores it.
  setHeadEffectVisible(false);
  try {
    live2dModel.motion(group, idx, MOTION_PRIORITY_FORCE);
  } catch (e) {
    oneShotPlaying = false;
    setHeadEffectVisible(true); // playback failed -> restore immediately
  }
}

/**
 * Play a random motion different from the current state's (used on tap).
 */
function playRandomOneShot() {
  if (!live2dModel) return;
  const stateMotion = STATE_MOTIONS[currentState] || STATE_MOTIONS.sleeping;
  const choices = ALL_MOTIONS.filter(([g, i]) => !(g === stateMotion[0] && i === stateMotion[1]));
  if (!choices.length) return;
  const [group, idx] = choices[Math.floor(Math.random() * choices.length)];
  playMotionOnce(group, idx);
}

/**
 * Build the motion list in the menu from ALL_MOTIONS.
 * - mode 'play'   : clicking a motion plays it once (one-shot).
 * - mode 'assign' : clicking a motion assigns it to `targetState` and returns
 *                   to the 动作设定 view; the current choice is checkmarked.
 */
function buildMotionMenu(mode = 'play', targetState = null) {
  const container = document.getElementById('motion-list');
  if (!container) return;
  container.innerHTML = '';
  const current = (mode === 'assign' && targetState) ? STATE_MOTIONS[targetState] : null;
  for (const [group, idx, name] of ALL_MOTIONS) {
    const item = document.createElement('div');
    item.className = 'menu-item';
    item.textContent = i18n.t(name);
    if (current && current[0] === group && current[1] === idx) {
      item.classList.add('active');
    }
    if (mode === 'assign' && targetState) {
      item.addEventListener('click', () => {
        assignStateMotion(targetState, group, idx);
        showView('menu-settings-view');
      });
    } else {
      item.addEventListener('click', () => {
        playMotionOnce(group, idx);
        closeMenu();
      });
    }
    container.appendChild(item);
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
 * Load persisted per-state motion assignments from the main process.
 */
async function loadStateMotions() {
  if (!window.petAPI || !window.petAPI.getStateMotions) return;
  try {
    const saved = await window.petAPI.getStateMotions();
    if (saved && typeof saved === 'object') {
      for (const state of ['sleeping', 'working', 'alert']) {
        const m = saved[state];
        if (Array.isArray(m) && m.length === 2 && typeof m[0] === 'string' && Number.isInteger(m[1])) {
          STATE_MOTIONS[state] = [m[0], m[1]];
        }
      }
    }
  } catch (err) {
    console.warn('[motions] Failed to load state motions:', err.message);
  }
}

/**
 * Populate the 动作设定 view with each state's current motion name.
 */
function buildSettingsMenu() {
  for (const state of ['sleeping', 'working', 'alert']) {
    const [g, i] = STATE_MOTIONS[state] || STATE_MOTIONS.sleeping;
    const found = ALL_MOTIONS.find(([mg, mi]) => mg === g && mi === i);
    const name = found ? i18n.t(found[2]) : `${g}[${i}]`;
    const el = document.getElementById(`settings-${state}-name`);
    if (el) el.textContent = name;
  }
}

/**
 * Assign a motion to a state, persist it, refresh the settings view, and if
 * the changed state is currently active, immediately switch to the new motion.
 */
function assignStateMotion(state, group, idx) {
  STATE_MOTIONS[state] = [group, idx];
  buildSettingsMenu();
  if (window.petAPI && window.petAPI.setStateMotions) {
    window.petAPI.setStateMotions(STATE_MOTIONS);
  }
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
    const { flipHorizontal: f } = await window.petAPI.getAppearance();
    flipHorizontal = !!f;
  } catch (err) {
    console.warn('[appearance] Failed to load:', err.message);
  }
}

/**
 * Apply the current flipHorizontal setting to the loaded model. scale.y holds
 * the positive base scale set in attachModel; flipping scale.x mirrors it.
 */
function applyFlip() {
  if (!live2dModel) return;
  const baseScale = live2dModel.scale.y;
  live2dModel.scale.x = flipHorizontal ? -baseScale : baseScale;
}

/**
 * Toggle horizontal mirror, persist it, and update the menu checkmark.
 */
function toggleFlip() {
  flipHorizontal = !flipHorizontal;
  applyFlip();
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
  console.log('[Fallback] Canvas animation started');
}

function getStateColor(state) {
  return STATE_COLORS[state] || STATE_COLORS.sleeping;
}

// ===== State Management =====
function setPetState(state) {
  if (currentState === state) return;
  console.log('[State] Transition:', currentState, '→', state);
  currentState = state;

  // Cancel any in-flight one-shot and immediately play the new state's motion
  // at FORCE priority so it overrides whatever was playing.
  oneShotPlaying = false;
  playStateMotion(MOTION_PRIORITY_FORCE);

  // Update UI
  updateStateUI(state);

  // Clear and restart effects
  clearEffects();

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

  // Shake on alert
  if (state === 'alert') {
    canvasWrapper.classList.add('shake');
  }
}

// ===== Effects Loop =====
function startEffectsLoop() {
  if (effectsTimer) clearInterval(effectsTimer);
  effectsTimer = setInterval(() => {
    // Safety net: keep the state motion looping even if motionFinish is missed.
    triggerPeriodicMotion();
    // Head-top effects (ZZZ / working dots / !) are driven by CSS animations
    // inside #head-effect and repositioned every frame by updateDebugBounds,
    // so this loop no longer spawns DOM nodes.
  }, EFFECT_INTERVAL_MS);
}

function clearEffects() {
  const overlay = document.getElementById('effect-overlay');
  overlay.innerHTML = '';
}

// ===== Status Bar Rendering =====
function updateStatusBar(snapshot) {
  currentSnapshot = snapshot;
  const projectList = document.getElementById('project-list');

  if (!snapshot.sessions || snapshot.sessions.length === 0) {
    projectList.innerHTML = `<div class="project-item empty">${i18n.t('status.waiting')}</div>`;
    return;
  }

  projectList.innerHTML = '';
  for (const session of snapshot.sessions) {
    const item = document.createElement('div');
    item.className = `project-item status-${session.status}`;

    const dot = document.createElement('div');
    dot.className = `project-dot ${session.status}`;

    const name = document.createElement('div');
    name.className = 'project-name';
    name.textContent = session.projectName;
    name.title = session.projectPath || session.projectName;

    const statusText = document.createElement('div');
    statusText.className = 'project-status-text';
    if (session.status === 'confirmation-needed') {
      statusText.classList.add('alert');
      statusText.textContent = session.alertMessage || i18n.t('status.confirmationNeeded');
    } else if (session.status === 'working') {
      statusText.textContent = i18n.t('status.busy');
    } else {
      statusText.textContent = i18n.t('status.idle');
    }

    item.appendChild(dot);
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
}

// ===== IPC Setup =====
function setupIPC() {
  if (!window.petAPI) {
    console.warn('[IPC] petAPI not available (preload not loaded)');
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

  window.petAPI.onAlert((snapshot) => {
    updateStatusBar(snapshot);
    setPetState('alert');
    // Flash window attention
    flashWindowAttention();
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
/**
 * Position the menu above the ☰ button, clamped to the window. The max-height
 * is capped to the space above the button so the tall motion submenu scrolls
 * internally instead of being clipped at the window bottom.
 */
function positionMenu() {
  const menuBtn = document.getElementById('menu-btn');
  const contextMenu = document.getElementById('context-menu');
  const rect = menuBtn.getBoundingClientRect();
  // Space available above the button (menu opens upward). Floor at 140 so a
  // tiny window still shows a usable menu.
  const maxH = Math.max(140, rect.top - 6);
  contextMenu.style.maxHeight = maxH + 'px';
  contextMenu.classList.remove('hidden');
  // Menu open: keep the whole window clickable so menu items stay interactive
  // and click-outside can close (the ticker also reports the menu rect, but
  // force-clickable covers the gap before the next ticker fire / on submenu
  // resize). Cleared in closeMenu().
  if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(true);
  // Measure after unhiding, then clamp upward so the bottom stays visible.
  const menuHeight = contextMenu.offsetHeight || 200;
  const menuWidth = contextMenu.offsetWidth || 210;
  const top = Math.max(2, rect.top - menuHeight - 4);
  const left = Math.max(0, rect.right - menuWidth);
  contextMenu.style.left = left + 'px';
  contextMenu.style.top = top + 'px';
}

/**
 * Re-clamp the menu's vertical position after switching views — the content
 * height differs between the main view and the motion submenu.
 */
function repositionMenu() {
  const menuBtn = document.getElementById('menu-btn');
  const contextMenu = document.getElementById('context-menu');
  if (contextMenu.classList.contains('hidden')) return;
  const rect = menuBtn.getBoundingClientRect();
  const menuHeight = contextMenu.offsetHeight || 200;
  contextMenu.style.top = Math.max(2, rect.top - menuHeight - 4) + 'px';
}

/**
 * Show one of the three menu views (main / model / motion), hiding the others.
 */
function showView(viewId) {
  for (const id of ['menu-main-view', 'menu-model-view', 'menu-motion-view', 'menu-settings-view', 'menu-language-view']) {
    document.getElementById(id).classList.toggle('hidden', id !== viewId);
  }
  repositionMenu();
}

/** Hide the menu and reset it to the main view for the next open. */
function closeMenu() {
  if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(false);
  document.getElementById('context-menu').classList.add('hidden');
  showView('menu-main-view');
}

function setupContextMenu() {
  const menuBtn = document.getElementById('menu-btn');
  const contextMenu = document.getElementById('context-menu');

  // Toggle open/close on ☰ click. Clicking the button doesn't bubble to the
  // document (stopPropagation), so without a toggle a second click would just
  // reposition the already-open menu.
  menuBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    if (!contextMenu.classList.contains('hidden')) {
      closeMenu();
      return;
    }
    showView('menu-main-view'); // always open on the main view
    positionMenu();
  });

  // Clicking anywhere outside the menu (blank space) closes it.
  document.addEventListener('click', () => {
    closeMenu();
  });

  // Window losing focus (user clicked another app) also closes the menu —
  // the transparent pet window is small and can't receive click events from
  // outside its bounds, so the document click handler above won't fire.
  window.addEventListener('blur', closeMenu);

  contextMenu.addEventListener('click', (e) => {
    e.stopPropagation();
  });

  // Secondary menus: 切换形象 / 播放动作 / 动作设定  ->  pop out the list
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

  // 检查新版本 -> check for updates via electron-updater.
  document.getElementById('menu-check-updates').addEventListener('click', () => {
    closeMenu();
    // If an update was already downloaded, clicking again installs it.
    if (updateDownloaded) {
      if (window.petAPI && window.petAPI.installUpdate) window.petAPI.installUpdate();
    } else {
      checkForUpdates();
    }
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

  // Menu actions
  document.getElementById('menu-install-hooks').addEventListener('click', async () => {
    closeMenu();
    if (window.petAPI) {
      const result = await window.petAPI.installHooks();
      if (result.success) {
        showInstallResult(true, result);
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

  document.getElementById('menu-uninstall').addEventListener('click', () => {
    if (window.petAPI && window.petAPI.uninstallApp) {
      window.petAPI.uninstallApp();
    }
  });
}

function showInstallResult(success, result) {
  const stateText = document.getElementById('pet-state-text');
  if (success) {
    stateText.textContent = i18n.t('hook.installSuccess');
    stateText.title = [
      i18n.t('hook.installSuccess'),
      '',
      'Trae IDE: Settings -> Hooks -> Local auto-run -> Enable -> New AI session',
      'Qoder: restart the IDE (hooks load automatically on startup)',
    ].join('\n');
    // Don't auto-reset; let the state-update replace it when events arrive.
  } else {
    stateText.textContent = i18n.t('hook.installFailed');
    setTimeout(() => { updateStateUI(currentState); }, 4000);
  }
}

async function checkHooksStatus() {
  if (!window.petAPI) return;
  const status = await window.petAPI.isHooksInstalled();
  hooksInstalled = !!status.installed;
  updateHookStatusHint(hooksInstalled, currentSnapshot);
  const stateText = document.getElementById('pet-state-text');
  if (!hooksInstalled) {
    if (currentSnapshot.sessions.length === 0 && currentState === 'sleeping') {
      stateText.textContent = i18n.t('hook.pleaseInstall');
      stateText.title = i18n.t('hook.pleaseInstall');
    }
  } else if (currentSnapshot.sessions.length === 0 && currentState === 'sleeping') {
    // Hooks installed but no events ever received → user needs to enable in IDE.
    stateText.textContent = i18n.t('hook.pleaseEnable');
    stateText.title = [
      i18n.t('hook.pleaseEnable'),
      '',
      '1. Trae IDE -> Settings -> Hooks',
      '2. Local auto-run',
      '3. Enable',
      '4. New AI session',
    ].join('\n');
  }
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
  ];
  stateText.textContent = status.installed ? i18n.t('hook.dialogInstalledMsg') : i18n.t('hook.dialogNotInstalledMsg');
  stateText.title = lines.join('\n') + '\n\n' + i18n.t('hook.dialogHint');
  setTimeout(() => {
    stateText.textContent = original;
    stateText.title = '';
  }, 4000);
}

// ===== Drag Setup =====
function setupDrag() {
  const wrapper = document.getElementById('canvas-wrapper');
  let lastX = 0;
  let lastY = 0;
  let hasMoved = false;

  wrapper.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return; // Only left click
    // Only start dragging when the pointer is actually over the model's bounds,
    // not on the empty canvas padding — matches the click-through hit area so
    // the "solid red box" interior is both the tap target and the drag target.
    if (!isPointOnModel(e.clientX, e.clientY)) return;
    isDragging = true;
    // Keep the window clickable for the whole drag — the cursor may leave the
    // model bounds mid-drag, which would otherwise flip the window to
    // click-through (via the Rust polling loop) and break dragging.
    if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(true);
    hasMoved = false;
    lastX = e.screenX;
    lastY = e.screenY;
  });

  document.addEventListener('mousemove', (e) => {
    if (!isDragging) return;
    const deltaX = e.screenX - lastX;
    const deltaY = e.screenY - lastY;
    if (Math.abs(deltaX) > 1 || Math.abs(deltaY) > 1) {
      hasMoved = true;
    }
    if (hasMoved && window.petAPI) {
      window.petAPI.dragWindow(deltaX, deltaY);
    }
    lastX = e.screenX;
    lastY = e.screenY;
  });

  document.addEventListener('mouseup', () => {
    isDragging = false;
    if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(false);
  });

  // Also support drag on the status bar
  const statusBar = document.getElementById('status-bar');
  statusBar.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    if (e.target.closest('.project-item') || e.target.closest('#menu-btn')) return;
    isDragging = true;
    if (window.petAPI && window.petAPI.setForceClickable) window.petAPI.setForceClickable(true);
    hasMoved = false;
    lastX = e.screenX;
    lastY = e.screenY;
  });
}

// ===== Click-Through =====
/**
 * Test whether a screen point (clientX/Y) is over the Live2D model's hit area.
 * Uses the model's current world-space bounding box (getBounds()), which is
 * the same rect as the red debug box — so the interactive area matches what
 * the user sees as the "solid red box" interior. Empty canvas padding outside
 * the box returns false and passes clicks through to the desktop.
 */
function isPointOnModel(clientX, clientY) {
  if (!live2dModel || !pixiApp) return false;
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
  if (window.__petSendLog) window.__petSendLog('info', '[clickThrough] region reporter registered (Rust polls GetCursorPos)');

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
    }
    const sb = document.getElementById('status-bar');
    if (sb) rects.push(toPhys(sb.getBoundingClientRect()));
    const cm = document.getElementById('context-menu');
    if (cm && !cm.classList.contains('hidden')) rects.push(toPhys(cm.getBoundingClientRect()));
    return rects;
  }

  // Report regions on the PixiJS ticker (rAF — keeps running even while the
  // window is click-through, unlike mousemove). Throttle to every 3rd frame
  // (~8Hz): getBounds() is relatively expensive and the CSS rects barely move.
  let frameSkip = 0;
  pixiApp.ticker.add(() => {
    frameSkip = (frameSkip + 1) % 3;
    if (frameSkip !== 0) return;
    window.petAPI.updateClickRegions(collectRegions());
  });

  // Seed regions immediately so the Rust polling thread (30ms) has data before
  // the first ticker fire (~125ms) — otherwise transparent areas would block
  // the desktop briefly at startup.
  window.petAPI.updateClickRegions(collectRegions());
}

// ===== Start =====
// Surface uncaught renderer errors to the main-process log for diagnostics.
window.addEventListener('error', (e) => {
  console.error('[uncaught]', e.message, e.filename + ':' + e.lineno);
});
window.addEventListener('unhandledrejection', (e) => {
  console.error('[unhandled rejection]', e.reason && e.reason.message ? e.reason.message : e.reason);
});

init();
