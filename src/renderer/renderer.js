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
let usingFallback = false;

// Model URLs to try (in order)
const MODEL_URLS = [
  '../../assets/live2d/nito.model3.json',
  'https://cdn.jsdelivr.net/gh/guansss/pixi-live2d-display/test/assets/hiyori/hiyori_pro_t10.model3.json',
];

// ===== Initialize =====
async function init() {
  setupIPC();
  setupContextMenu();
  setupDrag();

  // Wait for libraries to load
  const libsReady = await waitForLibs();

  if (libsReady) {
    await initLive2D();
  }

  if (!live2dModel) {
    // Fallback to canvas animation
    initFallbackCanvas();
    setPetState('sleeping');
    updateStateUI('sleeping');
  }

  // Start effects loop
  startEffectsLoop();

  // Check if hooks are installed
  checkHooksStatus();
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
async function initLive2D() {
  try {
    const PIXI = window.PIXI;
    const { Live2DModel } = PIXI.live2d;

    // Create PixiJS application
    pixiApp = new PIXI.Application({
      width: 300,
      height: 380,
      backgroundAlpha: 0,
      antialias: true,
      autoStart: true,
    });

    const canvasContainer = document.getElementById('live2d-canvas');
    canvasContainer.appendChild(pixiApp.view);

    // Try loading model from multiple sources
    for (const url of MODEL_URLS) {
      try {
        console.log('[Live2D] Trying model:', url);
        live2dModel = await Live2DModel.from(url);
        break;
      } catch (err) {
        console.warn('[Live2D] Failed to load from:', url, err.message);
      }
    }

    if (!live2dModel) {
      throw new Error('All model URLs failed');
    }

    // Scale and position the model
    const scale = Math.min(
      pixiApp.view.width / live2dModel.width,
      pixiApp.view.height / live2dModel.height
    ) * 0.9;

    live2dModel.scale.set(scale);
    live2dModel.anchor.set(0.5, 1);
    live2dModel.x = pixiApp.view.width / 2;
    live2dModel.y = pixiApp.view.height;

    pixiApp.stage.addChild(live2dModel);

    // Set up model interaction
    live2dModel.interactive = true;
    live2dModel.buttonMode = true;

    // Tap to play random motion
// Click interaction (nito model has no defined hit areas)
    live2dModel.on('pointerdown', () => {
      const motions = ['Tap', 'FlickUp', 'Shake'];
      const random = motions[Math.floor(Math.random() * motions.length)];
      live2dModel.motion(random);
    });

    // Override update for state-based parameter control
    const originalUpdate = live2dModel.internalModel.update.bind(live2dModel.internalModel);
    live2dModel.internalModel.update = function (dt) {
      originalUpdate(dt);
      applyStateParameters(this);
    };

    // Start with sleeping state
    setPetState('sleeping');

    console.log('[Live2D] Model loaded successfully');
  } catch (err) {
    console.error('[Live2D] Initialization failed:', err);
    live2dModel = null;
  }
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
  usingFallback = true;
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
  switch (state) {
    case 'sleeping': return '#7b9eff';
    case 'working': return '#ffc832';
    case 'alert': return '#ff6666';
    default: return '#7b9eff';
  }
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
          // Play idle motion
          live2dModel.motion('Idle');
          break;
        case 'working':
          live2dModel.motion('Tap');
          break;
        case 'alert':
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

  const labels = {
    sleeping: '💤 睡觉中',
    working: '⚡ 忙碌中',
    alert: '🔔 需要确认!',
  };
  stateText.textContent = labels[state] || state;

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
  }, 2000);
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
  // Only add if not already present
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
    contextMenu.style.left = (rect.right - 180) + 'px';
    contextMenu.style.top = (rect.bottom + 4) + 'px';
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
      } else {
        showInstallResult(false, result);
      }
    }
  });

  document.getElementById('menu-test-busy').addEventListener('click', () => {
    contextMenu.classList.add('hidden');
    setPetState('working');
    updateStateUI('working');
  });

  document.getElementById('menu-test-alert').addEventListener('click', () => {
    contextMenu.classList.add('hidden');
    setPetState('alert');
    updateStateUI('alert');
  });

  document.getElementById('menu-test-sleep').addEventListener('click', () => {
    contextMenu.classList.add('hidden');
    setPetState('sleeping');
    updateStateUI('sleeping');
  });

  document.getElementById('menu-quit').addEventListener('click', () => {
    if (window.petAPI) {
      window.petAPI.quit();
    }
  });
}

function showInstallResult(success, result) {
  const stateText = document.getElementById('pet-state-text');
  const original = stateText.textContent;
  if (success) {
    stateText.textContent = '✅ Hook 安装成功!';
  } else {
    stateText.textContent = '❌ Hook 安装失败';
  }
  setTimeout(() => {
    stateText.textContent = original;
  }, 3000);
}

async function checkHooksStatus() {
  if (!window.petAPI) return;
  const status = await window.petAPI.isHooksInstalled();
  if (!status.installed) {
    const stateText = document.getElementById('pet-state-text');
    stateText.textContent = '⚠ 请安装 Hook 集成';
  }
}

// ===== Drag Setup =====
function setupDrag() {
  const wrapper = document.getElementById('canvas-wrapper');
  let isDragging = false;
  let lastX = 0;
  let lastY = 0;
  let dragStartTime = 0;
  let hasMoved = false;

  wrapper.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return; // Only left click
    isDragging = true;
    hasMoved = false;
    lastX = e.screenX;
    lastY = e.screenY;
    dragStartTime = Date.now();
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
    dragStartTime = Date.now();
  });
}

// ===== Start =====
init();
