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
const { createServer } = require('./server');
const { initUpdater } = require('./updater');
const config = require('./config');

// Global references (prevent GC)
let mainWindow = null;
let stateManager = null;
let httpServer = null;

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

/**
 * Scan assets/live2d/ for available Live2D models (*.model3.json).
 * Returns [{ name, url }] where url is a renderer-relative path.
 */
function scanModels() {
  const fs = require('fs');
  const live2dDir = path.join(getAssetsDir(), 'live2d');
  const models = [];
  try {
    const entries = fs.readdirSync(live2dDir);
    for (const entry of entries) {
      if (entry.endsWith('.model3.json')) {
        const name = entry.replace(/\.model3\.json$/, '');
        models.push({ name, url: `../../assets/live2d/${entry}` });
      }
    }
  } catch (e) {
    console.error('[models] Failed to scan live2d dir:', e.message);
  }
  return models;
}

/**
 * Load persisted user preferences from ~/.trae-pet/config.json.
 */
function loadUserConfig() {
  const fs = require('fs');
  const os = require('os');
  const configPath = path.join(os.homedir(), '.trae-pet', 'config.json');
  try {
    if (fs.existsSync(configPath)) {
      return JSON.parse(fs.readFileSync(configPath, 'utf-8'));
    }
  } catch (e) {
    console.error('[config] Failed to read user config:', e.message);
  }
  return {};
}

/**
 * Persist user preferences to ~/.trae-pet/config.json.
 */
function saveUserConfig(userCfg) {
  const fs = require('fs');
  const os = require('os');
  const configDir = path.join(os.homedir(), '.trae-pet');
  const configPath = path.join(configDir, 'config.json');
  try {
    fs.mkdirSync(configDir, { recursive: true });
    fs.writeFileSync(configPath, JSON.stringify(userCfg, null, 2), 'utf-8');
  } catch (e) {
    console.error('[config] Failed to save user config:', e.message);
  }
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

/**
 * Check whether a window at (x,y) with size (w,h) is visible on any display.
 * Guards against restoring an off-screen position when monitors change.
 */
function isPositionOnScreen(x, y, w, h) {
  const cx = x + w / 2;
  const cy = y + h / 2;
  for (const d of screen.getAllDisplays()) {
    const b = d.bounds;
    if (cx >= b.x && cx <= b.x + b.width && cy >= b.y && cy <= b.y + b.height) return true;
  }
  return false;
}

function createMainWindow() {
  const { width: screenWidth, height: screenHeight } = screen.getPrimaryDisplay().workAreaSize;

  // Default to bottom-right; restore the last saved position if it's still on
  // a visible screen (handles monitor layout changes between runs).
  const userCfg = loadUserConfig();
  let x = screenWidth - config.WINDOW_WIDTH - config.WINDOW_MARGIN;
  let y = screenHeight - config.WINDOW_HEIGHT - config.WINDOW_MARGIN;
  if (userCfg.windowPosition
      && isPositionOnScreen(userCfg.windowPosition.x, userCfg.windowPosition.y, config.WINDOW_WIDTH, config.WINDOW_HEIGHT)) {
    x = userCfg.windowPosition.x;
    y = userCfg.windowPosition.y;
  }

  mainWindow = new BrowserWindow({
    width: config.WINDOW_WIDTH,
    height: config.WINDOW_HEIGHT,
    x,
    y,
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

  // Forward renderer console messages to main-process stdout for diagnostics.
  mainWindow.webContents.on('console-message', (event, level, message, line, sourceId) => {
    console.log(`[renderer] ${message}`);
  });
  mainWindow.webContents.on('render-process-gone', (event, details) => {
    console.error('[renderer] process gone:', JSON.stringify(details));
  });

  // Load renderer
  mainWindow.loadFile(path.join(__dirname, '..', 'renderer', 'index.html'));

  // Open DevTools in dev mode
  if (process.argv.includes('--dev')) {
    mainWindow.webContents.openDevTools({ mode: 'detach' });
  }

  mainWindow.on('closed', () => {
    mainWindow = null;
  });

  // Persist window position on move (debounced) so it restores on next launch.
  let moveTimer = null;
  mainWindow.on('move', () => {
    if (moveTimer) clearTimeout(moveTimer);
    moveTimer = setTimeout(() => {
      if (!mainWindow || mainWindow.isDestroyed()) return;
      const [px, py] = mainWindow.getPosition();
      const cfg = loadUserConfig();
      cfg.windowPosition = { x: px, y: py };
      saveUserConfig(cfg);
    }, 500);
  });

  return mainWindow;
}

/**
 * Periodically scan for open Trae IDE windows and register idle sessions for
 * their projects. This detects newly opened IDEs before any AI activity starts
 * — Trae IDE's SessionStart hook only fires when an AI agent session begins,
 * not when an IDE window opens, so without scanning a new IDE is invisible to
 * the pet until the user runs an AI task.
 */
/**
 * Periodically scan for open Trae IDE windows and register idle sessions.
 *
 * Adaptive interval: 4s when sessions exist (detect window close quickly),
 * 15s when no sessions (user probably has no IDE open, save CPU). Uses
 * setTimeout recursion instead of setInterval so a slow PowerShell run can't
 * overlap with the next tick.
 */
function startIdeScanner() {
  const { execFile } = require('child_process');
  // Enumerate ALL visible top-level windows via Win32 EnumWindows (not
  // Get-Process.MainWindowTitle, which only returns ONE title per process —
  // the foreground window — so a second IDE window was invisible to the
  // scanner). Trae CN window titles look like:
  //   "<active-file> - <project-folder> - Trae CN"
  // The "* - Trae CN" suffix filters out non-IDE windows (Trae Pet itself,
  // File Explorer, etc.) and the project is the second-to-last segment.
  const psScript = `
    if (-not ('WindowEnum' -as [type])) { Add-Type @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public class WindowEnum {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);
  [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetWindowTextLength(IntPtr hWnd);
  public static List<string> GetVisibleTitles() {
    var titles = new List<string>();
    EnumWindows((hWnd, lParam) => { if (IsWindowVisible(hWnd)) { int len = GetWindowTextLength(hWnd); if (len > 0) { var sb = new StringBuilder(len + 1); GetWindowText(hWnd, sb, sb.Capacity); titles.Add(sb.ToString()); } } return true; }, IntPtr.Zero);
    return titles;
  }
}
"@
    }
    [WindowEnum]::GetVisibleTitles() | Where-Object { $_ -like '* - Trae CN' } | ForEach-Object {
      $parts = $_ -split ' - '
      if ($parts.Count -ge 2) { $parts[-2] }
    }
  `;
  let scanTimer = null;

  const scan = () => {
    if (!stateManager) { scheduleNext(4000); return; }
    execFile('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', psScript], { timeout: 5000 }, (err, stdout) => {
      if (err || !stdout) {
        scheduleNext(4000);
        return;
      }
      // Filter empty/generic titles, dedupe.
      const names = [...new Set(
        stdout.split('\n').map((s) => s.trim()).filter((s) => s && s !== 'Trae' && s !== 'Trae CN')
      )];
      stateManager.syncDetectedWindows(names.map((projectName) => ({ projectName })));
      // Adaptive: 4s when IDEs are open (need fast close detection), 15s when idle.
      scheduleNext(stateManager.sessions.size > 0 ? 4000 : 15000);
    });
  };

  const scheduleNext = (delay) => {
    if (scanTimer) clearTimeout(scanTimer);
    scanTimer = setTimeout(scan, delay);
  };

  scan(); // run once immediately so an already-open IDE shows up right away
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

  // Start the IDE window scanner (detects new IDEs without AI activity).
  startIdeScanner();

  // Initialize auto-update (electron-updater). Wires up IPC handlers and
  // forwards update events to the renderer.
  initUpdater(mainWindow);
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
  const { execFile } = require('child_process');
  const projectName = path.basename(projectPath);

  // Fixed PowerShell script — NO user-data interpolation. Project name/path are
  // passed via env vars and read with $env: inside the script. Matching uses
  // literal -eq / IndexOf (never -like on user data), so names like O'Brien or
  // test[1] cannot break parsing or inject commands.
  const psScript = `
    $name = $env:TRAE_PET_FOCUS_NAME
    $windows = Get-Process | Where-Object { $_.ProcessName -like 'Trae*' -and $_.MainWindowTitle -ne '' }
    if (-not $windows) { exit 0 }
    $target = $null
    # 1. Exact match on the project segment (second-to-last " - " segment) of
    #    the title. Title format: "<active-file> - <project> - Trae CN".
    foreach ($w in $windows) {
      $parts = $w.MainWindowTitle -split ' - '
      $folder = if ($parts.Count -ge 2) { $parts[-2] } else { $parts[0] }
      if ($folder -eq $name) { $target = $w; break }
    }
    # 2. Fallback: case-insensitive literal substring match.
    if (-not $target) {
      foreach ($w in $windows) {
        if ($w.MainWindowTitle.IndexOf($name, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) { $target = $w; break }
      }
    }
    # 3. Last resort: activate any Trae window.
    if (-not $target) { $target = @($windows)[0] }
    Add-Type -AssemblyName Microsoft.VisualBasic
    [Microsoft.VisualBasic.Interaction]::AppActivate($target.Id)
  `;

  execFile(
    'powershell.exe',
    ['-NoProfile', '-NonInteractive', '-Command', psScript],
    {
      timeout: 5000,
      env: { ...process.env, TRAE_PET_FOCUS_NAME: projectName, TRAE_PET_FOCUS_PATH: projectPath },
    },
    (err) => {
      if (err && err.killed) {
        console.warn(`[bring-to-front] timed out for project "${projectName}"`);
      } else if (err) {
        console.warn(`[bring-to-front] powershell failed for "${projectName}":`, err.message);
      }
    }
  );
});

ipcMain.on('quit', () => {
  app.quit();
});

// Uninstall: launch the NSIS uninstaller (generated by electron-builder in the
// install directory) as a detached process, then quit. In dev mode, just quit.
// The uninstaller's customUnInstall macro (installer.nsh) kills any running
// TraePet instance so file locks don't block removal.
ipcMain.on('uninstall-app', () => {
  if (!app.isPackaged) {
    app.quit();
    return;
  }
  const fs = require('fs');
  const { spawn } = require('child_process');
  const installDir = path.dirname(process.execPath);
  // electron-builder NSIS generates "Uninstall <productName>.exe" in the
  // install dir. Glob for it so we don't hardcode the exact name.
  let uninstallerPath = null;
  try {
    const files = fs.readdirSync(installDir);
    const name = files.find((f) => /^uninstall.*\.exe$/i.test(f));
    if (name) uninstallerPath = path.join(installDir, name);
  } catch (e) {
    console.error('[uninstall] Failed to find uninstaller:', e.message);
  }
  if (uninstallerPath) {
    spawn(uninstallerPath, [], { detached: true, stdio: 'ignore' }).unref();
  } else {
    // Fallback: open Windows Apps & Features so the user can uninstall there.
    spawn('cmd', ['/c', 'start', 'ms-settings:appsfeatures'], { detached: true, stdio: 'ignore' }).unref();
  }
  app.quit();
});

let _flashTimeout = null;

ipcMain.on('flash-attention', () => {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  // skipTaskbar:true makes flashFrame invisible on Windows, so also surface the
  // window and pulse opacity as a reliable visible attention signal.
  mainWindow.flashFrame(true);
  mainWindow.showInactive();

  // Cancel any in-progress flash so rapid alerts don't stack timers that
  // restore opacity to an intermediate (wrong) value.
  if (_flashTimeout) clearTimeout(_flashTimeout);

  const origOpacity = mainWindow.getOpacity();
  mainWindow.setOpacity(0.4);
  _flashTimeout = setTimeout(() => {
    _flashTimeout = null;
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.setOpacity(origOpacity);
    }
  }, 200);
});

ipcMain.handle('install-hooks', async () => {
  return await installHooks();
});

ipcMain.handle('is-hooks-installed', async () => {
  return await checkHooksInstalled();
});

// Return available Live2D models and the persisted current choice.
ipcMain.handle('get-models', () => {
  const models = scanModels();
  const userCfg = loadUserConfig();
  return { models, currentModelUrl: userCfg.modelUrl || null };
});

// Persist the user's model choice (~/.trae-pet/config.json).
ipcMain.on('switch-model', (_, modelUrl) => {
  const userCfg = loadUserConfig();
  userCfg.modelUrl = modelUrl;
  saveUserConfig(userCfg);
});

// Return persisted per-state motion assignments (sleeping/working/alert).
ipcMain.handle('get-state-motions', () => {
  const userCfg = loadUserConfig();
  return userCfg.stateMotions || {};
});

// Persist per-state motion assignments.
ipcMain.on('set-state-motions', (_, motions) => {
  const userCfg = loadUserConfig();
  userCfg.stateMotions = motions;
  saveUserConfig(userCfg);
});

// Return persisted appearance settings (horizontal flip, etc.).
ipcMain.handle('get-appearance', () => {
  const userCfg = loadUserConfig();
  return { flipHorizontal: !!userCfg.flipHorizontal };
});

// Persist the horizontal-flip toggle.
ipcMain.on('set-flip-horizontal', (_, enabled) => {
  const userCfg = loadUserConfig();
  userCfg.flipHorizontal = !!enabled;
  saveUserConfig(userCfg);
});

// Return the persisted UI language; falls back to the OS locale (via
// app.getLocale()) on first launch so the pet starts in the user's language.
ipcMain.handle('get-language', () => {
  const userCfg = loadUserConfig();
  if (userCfg.language) return userCfg.language;
  return app.getLocale() || 'en';
});

// Persist the user's language choice (~/.trae-pet/config.json).
ipcMain.on('set-language', (_, lang) => {
  const userCfg = loadUserConfig();
  userCfg.language = lang;
  saveUserConfig(userCfg);
});

// ===== Auto-launch (start on boot) =====
// Reads/writes the Windows Run registry key via Electron's cross-platform API.
// The NSIS installer also writes this key if the user opts in during install.
ipcMain.handle('get-auto-launch', () => {
  return app.getLoginItemSettings().openAtLogin;
});

ipcMain.on('set-auto-launch', (_, enabled) => {
  app.setLoginItemSettings({ openAtLogin: !!enabled });
});

// Demo: inject a fake confirmation-needed session so the user can preview the
// alert state (motion + UI). Auto-cleared after 8 seconds.
ipcMain.on('test-alert', () => {
  if (!stateManager) return;
  const testSessionId = '__trae-pet-test-alert__';
  stateManager.handleHookEvent({
    session_id: testSessionId,
    hook_event_name: 'Notification',
    project_path: '',
    project_name: '预览提醒',
    notification_type: 'permission_request',
    message: '预览提醒效果',
  });
  setTimeout(() => {
    if (stateManager) stateManager.removeSession(testSessionId);
  }, 8000);
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

  // Build the hook command (PowerShell call to the bridge script).
  // Use $env:USERPROFILE form for portability across machines/profiles,
  // matching hooks/hooks-template.json.
  const hookCommand = `& "$env:USERPROFILE\\.trae-pet\\hooks\\trae-hook-bridge.ps1"`;

  // Build hook entries from the centralized event list (config.HOOK_EVENTS).
  const traePetHooks = {};
  for (const eventName of config.HOOK_EVENTS) {
    traePetHooks[eventName] = [{
      hooks: [{ type: 'command', command: hookCommand, timeout: config.BRIDGE_TIMEOUT_SEC }],
    }];
  }

  // Merge: preserve existing hooks that aren't ours
  if (!existingHooks.version) existingHooks.version = 1;
  if (!existingHooks.hooks) existingHooks.hooks = {};

  // NOTE: exec_env is NOT a valid hooks.json field. The execution mode
  // (sandbox vs local) is set in Trae IDE's Settings -> Hooks -> 运行方式.
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

  // Auto-open hooks.json in the default editor (which should be Trae IDE if
  // it's the currently open editor for .json files). This causes Trae to detect
  // the new/changed hooks.json and prompt the user to enable it via the
  // security warning panel. If Trae doesn't open it, the user can manually
  // navigate to Settings → Hooks.
  const { shell } = require('electron');
  setTimeout(() => {
    shell.openPath(traeHooksPath).catch(() => { /* ignore open errors */ });
  }, 500);

  return { success: true, hookDir: targetHookDir, hooksPath: traeHooksPath, needsEnable: true };
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
  // Persist the final window position so it restores on next launch.
  if (mainWindow && !mainWindow.isDestroyed()) {
    const [px, py] = mainWindow.getPosition();
    const cfg = loadUserConfig();
    cfg.windowPosition = { x: px, y: py };
    saveUserConfig(cfg);
  }
});

} // end of single-instance lock block
