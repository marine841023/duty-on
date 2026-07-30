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

// Model URLs to try (in order)
const MODEL_URLS = [
  '../../assets/live2d/nito.model3.json',
  'https://cdn.jsdelivr.net/gh/guansss/pixi-live2d-display/test/assets/hiyori/hiyori_pro_t10.model3.json',
];

// ===== Constants =====
const PIXI_WIDTH = 300;
const PIXI_HEIGHT = 380;
const EFFECT_INTERVAL_MS = 2000;
const MOTION_NAMES = ['Tap', 'FlickUp', 'Shake'];
const STATE_LABELS = {
  sleeping: '💤 睡觉中',
  working: '⚡ 忙碌中',
  alert: '🔔 需要确认!',
};
const STATE_COLORS = {
  sleeping: '#7b9eff',
  working: '#ffc832',
  alert: '#ff6666',
};

// ===== Initialize =====
async function init() {
  setupIPC();
  setupContextMenu();
  setupDrag();

  // Fetch available models + persisted choice from the main process, then
  // build the model-switching menu.
  await loadModelCatalog();

  // Wait for libraries to load
  const libsReady = await waitForLibs();

  if (libsReady) {
    await initLive2D();
  }

  if (!live2dModel) {
    // Fallback to canvas animation
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
    currentModelUrl = saved || (availableModels[0] && availableModels[0].url) || null;
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
      document.getElementById('context-menu').classList.add('hidden');
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
    window.addEventListener('libs-ready', () => resolve(true), { once: true });
    window.addEventListener('libs-failed', () => resolve(false), { once: true });

    // If already loaded (script finished before listener attached)
    if (window.PIXI && window.PIXI.live2d) {
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
    initPixiApp();
    await loadInitialModel();
  } catch (err) {
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
  });
  const canvasContainer = document.getElementById('live2d-canvas');
  canvasContainer.appendChild(pixiApp.view);
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
      console.log('[Live2D] Trying model:', url);
      live2dModel = await Live2DModel.from(url);
      currentModelUrl = url;
      attachModel(live2dModel);
      setPetState('sleeping');
      console.log('[Live2D] Model loaded successfully:', url);
      return;
    } catch (err) {
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
  ) * 0.9;

  model.scale.set(scale);
  model.anchor.set(0.5, 1);
  model.x = pixiApp.view.width / 2;
  model.y = pixiApp.view.height;

  pixiApp.stage.addChild(model);

  model.interactive = true;
  model.buttonMode = true;

  // Tap to play random motion (nito model has no defined hit areas)
  model.on('pointerdown', () => {
    const random = MOTION_NAMES[Math.floor(Math.random() * MOTION_NAMES.length)];
    model.motion(random);
  });

  // Override update for state-based parameter control
  const originalUpdate = model.internalModel.update.bind(model.internalModel);
  model.internalModel.update = function (dt) {
    originalUpdate(dt);
    applyStateParameters(this);
  };
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
  stateText.textContent = '⏳ 切换形象中...';

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
    setPetState(currentState);
    updateModelMenuActive(url);
    if (window.petAPI && window.petAPI.switchModel) {
      window.petAPI.switchModel(url);
    }
    console.log('[Live2D] Switched successfully:', url);
  } catch (err) {
    console.error('[Live2D] Switch failed, falling back:', err.message);
    // Restore the previous model if the new one failed to load
    await loadInitialModel();
    stateText.textContent = '❌ 形象加载失败，已回退';
    setTimeout(() => { stateText.textContent = prevText; }, 2000);
    return;
  }
  stateText.textContent = prevText;
}

/**
 * Apply Live2D parameters based on current state.
 * Called every frame after the model's internal update.
 */
function applyStateParameters(internalModel) {
  const coreModel = internalModel.coreModel;
  if (!coreModel) return;

  try {
    switch (currentState) {
      case 'sleeping':
        setParam(coreModel, 'PARAM_EYE_L_OPEN', 0.05);
        setParam(coreModel, 'PARAM_EYE_R_OPEN', 0.05);
        setParam(coreModel, 'PARAM_MOUTH_OPEN_Y', 0);
        setParam(coreModel, 'PARAM_MOUTH_FORM', 0);
        setParam(coreModel, 'PARAM_ANGLE_Z', -5);
        setParam(coreModel, 'PARAM_SURP_ON', 0);
        setParam(coreModel, 'PARAM_ANGER_ON', 0);
        break;

      case 'working':
        setParam(coreModel, 'PARAM_EYE_L_OPEN', 1);
        setParam(coreModel, 'PARAM_EYE_R_OPEN', 1);
        setParam(coreModel, 'PARAM_MOUTH_FORM', 0.5);
        setParam(coreModel, 'PARAM_MOUTH_OPEN_Y', 0.1);
        setParam(coreModel, 'PARAM_EYE_BALL_Y', -0.3);
        setParam(coreModel, 'PARAM_SURP_ON', 0);
        break;

      case 'alert':
        setParam(coreModel, 'PARAM_EYE_L_OPEN', 1);
        setParam(coreModel, 'PARAM_EYE_R_OPEN', 1);
        setParam(coreModel, 'PARAM_SURP_ON', 1);
        setParam(coreModel, 'PARAM_MOUTH_OPEN_Y', 0.5);
        setParam(coreModel, 'PARAM_EYE_BALL_Y', 0.2);
        break;
    }
  } catch (e) {
    // Some parameters may not exist on all models
  }
}
/**
 * Safely set a Live2D parameter value.
 */
function setParam(coreModel, id, value) {
  try {
    coreModel.setParameterValueById(id, value);
  } catch (e) {
    // Parameter doesn't exist on this model, skip.
  }
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

  // Update Live2D motions
  if (live2dModel) {
    try {
      switch (state) {
        case 'sleeping':
          live2dModel.motion('Idle');
          break;
        case 'working':
        case 'alert':
          // Both states use the Tap motion; visual differentiation comes from
          // applyStateParameters (eye/mouth params) and the CSS shake on alert.
          live2dModel.motion('Tap');
          break;
      }
    } catch (e) {
      // Motion might not exist
    }
  }

  // Update UI
  updateStateUI(state);

  // Clear and restart effects
  clearEffects();
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

  stateText.textContent = STATE_LABELS[state] || state;

  // Shake on alert
  if (state === 'alert') {
    canvasWrapper.classList.add('shake');
  }
}

// ===== Effects Loop =====
function startEffectsLoop() {
  if (effectsTimer) clearInterval(effectsTimer);
  effectsTimer = setInterval(() => {
    switch (currentState) {
      case 'sleeping':
        spawnZZZ();
        break;
      case 'working':
        spawnSparkle();
        break;
      case 'alert':
        spawnAlertBang();
        break;
    }
  }, EFFECT_INTERVAL_MS);
}

function clearEffects() {
  const overlay = document.getElementById('effect-overlay');
  overlay.innerHTML = '';
}

function spawnZZZ() {
  const overlay = document.getElementById('effect-overlay');
  const zzz = document.createElement('div');
  zzz.className = 'zzz';
  zzz.textContent = 'Z';
  zzz.style.left = (140 + Math.random() * 40) + 'px';
  zzz.style.top = (80 + Math.random() * 20) + 'px';
  zzz.style.fontSize = (16 + Math.random() * 8) + 'px';
  overlay.appendChild(zzz);
  setTimeout(() => zzz.remove(), 3000);
}

function spawnSparkle() {
  const overlay = document.getElementById('effect-overlay');
  const sparkle = document.createElement('div');
  sparkle.className = 'sparkle';
  sparkle.style.left = (100 + Math.random() * 100) + 'px';
  sparkle.style.top = (150 + Math.random() * 50) + 'px';
  overlay.appendChild(sparkle);
  setTimeout(() => sparkle.remove(), 1500);
}

function spawnAlertBang() {
  const overlay = document.getElementById('effect-overlay');
  // Guard against duplicate '!' icons if the interval fires again while
  // alert state persists — the element is only removed by clearEffects() on
  // state transition.
  if (overlay.querySelector('.alert-bang')) return;
  const bang = document.createElement('div');
  bang.className = 'alert-bang';
  bang.textContent = '!';
  overlay.appendChild(bang);
}

// ===== Status Bar Rendering =====
function updateStatusBar(snapshot) {
  currentSnapshot = snapshot;
  const projectList = document.getElementById('project-list');

  if (!snapshot.sessions || snapshot.sessions.length === 0) {
    projectList.innerHTML = '<div class="project-item empty">等待 Trae IDE 连接...</div>';
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
      statusText.textContent = session.alertMessage || '需要确认';
    } else if (session.status === 'working') {
      statusText.textContent = '忙碌';
    } else {
      statusText.textContent = '空闲';
    }

    item.appendChild(dot);
    item.appendChild(name);
    item.appendChild(statusText);

    // Click to bring IDE to front
    item.addEventListener('click', () => {
      if (window.petAPI && session.projectPath) {
        window.petAPI.bringToFront(session.projectPath);
      }
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
function setupContextMenu() {
  const menuBtn = document.getElementById('menu-btn');
  const contextMenu = document.getElementById('context-menu');

  menuBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    const rect = menuBtn.getBoundingClientRect();
    contextMenu.classList.remove('hidden');
    // The ☰ button sits near the bottom of the 520px window, so open the menu
    // UPWARD (its bottom aligns just above the button). This keeps the 退出
    // item (last in the menu) visible right above the button instead of being
    // clipped off the bottom of the window. Clamp so it never overflows the top.
    const menuHeight = contextMenu.offsetHeight || 400;
    const menuWidth = contextMenu.offsetWidth || 210;
    const top = Math.max(2, rect.top - menuHeight - 4);
    const left = Math.max(0, rect.right - menuWidth);
    contextMenu.style.left = left + 'px';
    contextMenu.style.top = top + 'px';
  });

  document.addEventListener('click', () => {
    contextMenu.classList.add('hidden');
  });

  contextMenu.addEventListener('click', (e) => {
    e.stopPropagation();
  });

  // Menu actions
  document.getElementById('menu-install-hooks').addEventListener('click', async () => {
    contextMenu.classList.add('hidden');
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

  // Menu actions
  document.getElementById('menu-install-hooks').addEventListener('click', async () => {
    contextMenu.classList.add('hidden');
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
    contextMenu.classList.add('hidden');
    if (!window.petAPI) return;
    const status = await window.petAPI.isHooksInstalled();
    showHookStatusDialog(status);
  });

  // Test-state buttons: change the state through the normal setPetState path
  // (which cascades into motion, UI, and effects) but DON'T emit to the main
  // process StateManager — these are purely local visual overrides for dev.
  document.getElementById('menu-test-busy').addEventListener('click', () => {
    contextMenu.classList.add('hidden');
    setPetState('working');
  });

  document.getElementById('menu-test-alert').addEventListener('click', () => {
    contextMenu.classList.add('hidden');
    setPetState('alert');
  });

  document.getElementById('menu-test-sleep').addEventListener('click', () => {
    contextMenu.classList.add('hidden');
    setPetState('sleeping');
  });

  document.getElementById('menu-play-motion').addEventListener('click', () => {
    contextMenu.classList.add('hidden');
    if (live2dModel) {
      const random = MOTION_NAMES[Math.floor(Math.random() * MOTION_NAMES.length)];
      try {
        live2dModel.motion(random);
      } catch (e) {
        // Motion name may not exist on this model — try 'Idle' as fallback
        try { live2dModel.motion('Idle'); } catch (e2) { /* ignore */ }
      }
    }
  });

  document.getElementById('menu-quit').addEventListener('click', () => {
    if (window.petAPI) {
      window.petAPI.quit();
    }
  });
}

function showInstallResult(success, result) {
  const stateText = document.getElementById('pet-state-text');
  if (success) {
    // Step-by-step guide: files are written, but Trae IDE requires manual
    // enablement via Settings → Hooks → 启用 button.
    stateText.textContent = '✅ 已写入配置 → 请在 Trae 设置→Hooks 中启用';
    stateText.title = [
      'Hook 配置文件已写入，但 Trae IDE 需要在设置中启用后才会执行：',
      '',
      '1. Trae IDE 已打开 hooks.json（自动弹出）',
      '2. 在 Trae IDE 中按 Ctrl+, 打开设置',
      '3. 搜索 "Hooks" 进入 Hooks 设置页',
      '4. 在"全局"标签下找到已配置的 Hooks，点击齿轮→启用',
      '   （或点"创建"按钮让 IDE 自动识别已有配置）',
      '5. 在弹出的安全警示面板中点"启用"',
      '6. 开一个新的 AI 会话即可触发',
    ].join('\n');
    // Don't auto-reset; let the state-update replace it when events arrive.
  } else {
    stateText.textContent = '❌ Hook 安装失败';
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
      stateText.textContent = '⚠ 请点☰→安装 Hook 集成';
      stateText.title = '点右上角☰菜单 → 安装 Hook 集成';
    }
  } else if (currentSnapshot.sessions.length === 0 && currentState === 'sleeping') {
    // Hooks installed but no events ever received → user needs to enable in IDE.
    stateText.textContent = '⚠ 请在 Trae 设置→Hooks 中启用';
    stateText.title = [
      'Hook 配置文件已写入，但 Trae IDE 需要在设置中手动启用：',
      '',
      '1. 按 Ctrl+, 打开 Trae 设置',
      '2. 搜索 "Hooks" 进入 Hooks 设置页',
      '3. 全局标签下 → 点"创建"或齿轮图标→启用',
      '4. 弹出安全警示面板 → 点"启用"',
      '5. 开一个新的 AI 会话',
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
    parts.push('未安装');
  } else if (snapshot && snapshot.lastEventAt) {
    const ago = Math.max(0, Math.floor((Date.now() - snapshot.lastEventAt) / 1000));
    parts.push('已连接');
    parts.push(ago < 60 ? `${ago}s前` : `${Math.floor(ago / 60)}m前`);
  } else {
    parts.push('需在IDE启用');
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
    `Hook 配置: ${status.installed ? '已安装' : '未安装'}`,
    `bridge 脚本: ${status.bridgeExists ? '存在' : '缺失'}`,
    `hooks.json: ${status.hooksExist ? '存在' : '缺失'}`,
  ];
  stateText.textContent = status.installed ? '✅ Hook 已安装' : '⚠ Hook 未安装';
  stateText.title = lines.join('\n') + '\n\n若 IDE 不显示：设置→Hooks 启用 → 开新 AI 会话';
  setTimeout(() => {
    stateText.textContent = original;
    stateText.title = '';
  }, 4000);
}

// ===== Drag Setup =====
function setupDrag() {
  const wrapper = document.getElementById('canvas-wrapper');
  let isDragging = false;
  let lastX = 0;
  let lastY = 0;
  let hasMoved = false;

  wrapper.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return; // Only left click
    isDragging = true;
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
  });

  // Also support drag on the status bar
  const statusBar = document.getElementById('status-bar');
  statusBar.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    if (e.target.closest('.project-item') || e.target.closest('#menu-btn')) return;
    isDragging = true;
    hasMoved = false;
    lastX = e.screenX;
    lastY = e.screenY;
  });
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
