/**
 * Electron Main Process
 * - Creates transparent, always-on-top, frameless window
 * - Starts HTTP server for Trae IDE hook events
 * - Manages state and forwards updates to renderer
 * - Handles IPC from renderer (install hooks, drag window, etc.)
 */

const { app, BrowserWindow, ipcMain, screen } = require('electron');
const path = require('path');
const { StateManager } = require('./state-manager');
const { createServer, PORT } = require('./server');

// Global references (prevent GC)
let mainWindow = null;
let stateManager = null;
let httpServer = null;

const WINDOW_WIDTH = 320;
const WINDOW_HEIGHT = 520;

/**
 * Get the path to the hooks directory.
 * In dev: <project>/hooks/
 * In packaged: <resourcesPath>/hooks/ (via extraResources)
 */
function getHooksDir() {
  if (app.isPackaged) {
    return path.join(process.resourcesPath, 'hooks');
  }
  return path.join(__dirname, '..', '..', 'hooks');
}

/**
 * Get the path to the assets directory.
 * In dev: <project>/assets/
 * In packaged: assets are packed inside the asar, so use __dirname relative path
 */
function getAssetsDir() {
  return path.join(__dirname, '..', '..', 'assets');
}

// Single instance lock - prevent multiple pet instances
const gotTheLock = app.requestSingleInstanceLock();
if (!gotTheLock) {
  app.quit();
} else {

app.on('second-instance', () => {
  // Someone tried to run a second instance, focus our window instead
  if (mainWindow) {
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.focus();
  }
});

function createMainWindow() {
  const { width: screenWidth, height: screenHeight } = screen.getPrimaryDisplay().workAreaSize;

  mainWindow = new BrowserWindow({
    width: WINDOW_WIDTH,
    height: WINDOW_HEIGHT,
    x: screenWidth - WINDOW_WIDTH - 20,   // Bottom-right corner
    y: screenHeight - WINDOW_HEIGHT - 20,
    transparent: true,
    frame: false,
    alwaysOnTop: true,
    resizable: false,
    maximizable: false,
    minimizable: false,
    fullscreenable: false,
    skipTaskbar: true,                     // Don't show in taskbar
    hasShadow: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  // Set always-on-top level to screen-saver (highest, above normal windows)
  mainWindow.setAlwaysOnTop(true, 'screen-saver');

  // Load renderer
  mainWindow.loadFile(path.join(__dirname, '..', 'renderer', 'index.html'));

  // Open DevTools in dev mode
  if (process.argv.includes('--dev')) {
    mainWindow.webContents.openDevTools({ mode: 'detach' });
  }

  mainWindow.on('closed', () => {
    mainWindow = null;
  });

  return mainWindow;
}

app.whenReady().then(() => {
  // Initialize state manager
  stateManager = new StateManager();
  stateManager.startCleanupTimer();

  // Forward state updates to renderer
  stateManager.on('update', (snapshot) => {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('state-update', snapshot);
    }
  });

  stateManager.on('alert', (snapshot) => {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('alert', snapshot);
    }
  });

  // Start HTTP server
  httpServer = createServer(stateManager);

  // Create window
  createMainWindow();
});

// IPC Handlers
ipcMain.on('drag-window', (_, deltaX, deltaY) => {
  if (mainWindow) {
    const [x, y] = mainWindow.getPosition();
    mainWindow.setPosition(x + deltaX, y + deltaY);
  }
});

ipcMain.on('set-click-through', (_, ignore) => {
  if (mainWindow) {
    mainWindow.setIgnoreMouseEvents(ignore, { forward: true });
  }
});

ipcMain.on('bring-to-front', async (_, projectPath) => {
  if (!projectPath) return;
  try {
    const { exec } = require('child_process');
    const projectName = path.basename(projectPath);
    const psScript = `
      Add-Type -AssemblyName Microsoft.VisualBasic
      $windows = Get-Process | Where-Object { $_.MainWindowTitle -like '*${projectName}*' -and $_.ProcessName -like '*Trae*' }
      if ($windows) {
        [Microsoft.VisualBasic.Interaction]::AppActivate($windows[0].Id)
      }
    `;
    exec(`powershell -Command "${psScript.replace(/"/g, '\\"')}"`, { timeout: 5000 });
  } catch (e) {
    console.error('Failed to bring window to front:', e);
  }
});

ipcMain.on('quit', () => {
  app.quit();
});

ipcMain.handle('install-hooks', async () => {
  return await installHooks();
});

ipcMain.handle('is-hooks-installed', async () => {
  return await checkHooksInstalled();
});

/**
 * Install the hook bridge script and hooks.json for Trae IDE.
 * Works in both dev and packaged modes.
 */
async function installHooks() {
  const fs = require('fs');
  const os = require('os');
  const userHome = os.homedir();

  // 1. Locate the bridge script source
  const hooksDir = getHooksDir();
  const bridgeSrc = path.join(hooksDir, 'trae-hook-bridge.ps1');

  if (!fs.existsSync(bridgeSrc)) {
    return { success: false, error: `Bridge script not found: ${bridgeSrc}` };
  }

  // 2. Copy bridge script to ~/.trae-pet/hooks/
  const targetHookDir = path.join(userHome, '.trae-pet', 'hooks');
  fs.mkdirSync(targetHookDir, { recursive: true });
  const bridgeDst = path.join(targetHookDir, 'trae-hook-bridge.ps1');

  // Read from source (handles asar in packaged mode) and write to destination
  const bridgeContent = fs.readFileSync(bridgeSrc, 'utf-8');
  fs.writeFileSync(bridgeDst, bridgeContent, 'utf-8');

  // 3. Also copy the standalone installer script
  const installerSrc = path.join(hooksDir, 'install-hooks.ps1');
  if (fs.existsSync(installerSrc)) {
    const installerContent = fs.readFileSync(installerSrc, 'utf-8');
    fs.writeFileSync(path.join(targetHookDir, 'install-hooks.ps1'), installerContent, 'utf-8');
  }

  // 4. Create/update hooks.json at ~/.trae-cn/hooks.json
  const traeHooksPath = path.join(userHome, '.trae-cn', 'hooks.json');
  fs.mkdirSync(path.dirname(traeHooksPath), { recursive: true });

  // Read existing hooks if present, then merge
  let existingHooks = {};
  if (fs.existsSync(traeHooksPath)) {
    try {
      existingHooks = JSON.parse(fs.readFileSync(traeHooksPath, 'utf-8'));
    } catch (e) {
      console.error('Failed to parse existing hooks.json, creating new one');
    }
  }

  // Build the hook command (PowerShell call to the bridge script)
  const hookCommand = `& "${bridgeDst}"`;

  const traePetHooks = {
    SessionStart: [{ hooks: [{ type: 'command', command: hookCommand, timeout: 5 }] }],
    UserPromptSubmit: [{ hooks: [{ type: 'command', command: hookCommand, timeout: 5 }] }],
    PreToolUse: [{ hooks: [{ type: 'command', command: hookCommand, timeout: 5 }] }],
    PostToolUse: [{ hooks: [{ type: 'command', command: hookCommand, timeout: 5 }] }],
    Stop: [{ hooks: [{ type: 'command', command: hookCommand, timeout: 5 }] }],
    Notification: [{ hooks: [{ type: 'command', command: hookCommand, timeout: 5 }] }],
  };

  // Merge: preserve existing hooks that aren't ours
  if (!existingHooks.version) existingHooks.version = 1;
  if (!existingHooks.hooks) existingHooks.hooks = {};

  for (const [eventName, hookGroups] of Object.entries(traePetHooks)) {
    if (!existingHooks.hooks[eventName]) {
      existingHooks.hooks[eventName] = [];
    }
    // Remove any existing trae-pet hook groups (avoid duplicates)
    existingHooks.hooks[eventName] = existingHooks.hooks[eventName].filter(
      (group) => !group.hooks?.some((h) => h.command?.includes('.trae-pet'))
    );
    // Add our hook group
    existingHooks.hooks[eventName].push(...hookGroups);
  }

  fs.writeFileSync(traeHooksPath, JSON.stringify(existingHooks, null, 2), 'utf-8');

  return { success: true, hookDir: targetHookDir, hooksPath: traeHooksPath };
}

/**
 * Check if hooks are already installed.
 */
async function checkHooksInstalled() {
  const fs = require('fs');
  const os = require('os');
  const userHome = os.homedir();
  const traeHooksPath = path.join(userHome, '.trae-cn', 'hooks.json');
  const bridgePath = path.join(userHome, '.trae-pet', 'hooks', 'trae-hook-bridge.ps1');

  const hooksExist = fs.existsSync(traeHooksPath);
  const bridgeExists = fs.existsSync(bridgePath);

  let hooksContainPet = false;
  if (hooksExist) {
    try {
      const content = fs.readFileSync(traeHooksPath, 'utf-8');
      hooksContainPet = content.includes('.trae-pet');
    } catch (e) { /* ignore */ }
  }

  return {
    installed: hooksContainPet && bridgeExists,
    hooksExist,
    bridgeExists,
  };
}

// App lifecycle
app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    if (stateManager) stateManager.stop();
    if (httpServer) httpServer.close();
    app.quit();
  }
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createMainWindow();
  }
});

app.on('before-quit', () => {
  if (stateManager) stateManager.stop();
  if (httpServer) httpServer.close();
});

} // end of single-instance lock block
