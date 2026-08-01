/**
 * Auto-update module — wraps electron-updater and forwards events to the
 * renderer via IPC.
 *
 * The update server URL is configured in package.json `build.publish` and
 * baked into app-update.yml at build time. To change it after build, edit
 * resources/app-update.yml in the installed directory.
 *
 * In dev mode (app.isPackaged === false), update checks are no-ops —
 * electron-updater needs the packaged asar + app-update.yml to work.
 */

const { app, ipcMain } = require('electron');
let autoUpdater = null;

try {
  ({ autoUpdater } = require('electron-updater'));
} catch (err) {
  console.warn('[Updater] electron-updater not available:', err.message);
}

let mainWindow = null;

/**
 * Initialize the updater with the main window reference and wire up IPC.
 * Called once from index.js after the window is created.
 */
function initUpdater(window) {
  mainWindow = window;

  if (!autoUpdater) {
    console.warn('[Updater] autoUpdater unavailable — update features disabled');
    return;
  }

  // Don't auto-install on quit; let the user trigger it from the menu.
  autoUpdater.autoDownload = true;
  autoUpdater.autoInstallOnAppQuit = false;

  // Forward all updater events to the renderer.
  autoUpdater.on('checking-for-update', () => {
    send('update-status', { status: 'checking' });
  });

  autoUpdater.on('update-available', (info) => {
    send('update-status', { status: 'available', version: info.version, releaseDate: info.releaseDate });
  });

  autoUpdater.on('update-not-available', (info) => {
    send('update-status', { status: 'not-available', version: info.version });
  });

  autoUpdater.on('download-progress', (progress) => {
    send('update-status', {
      status: 'downloading',
      percent: Math.round(progress.percent),
      transferred: progress.transferred,
      total: progress.total,
    });
  });

  autoUpdater.on('update-downloaded', (info) => {
    send('update-status', { status: 'downloaded', version: info.version });
  });

  autoUpdater.on('error', (err) => {
    const msg = (err && err.message) || String(err);
    // Classify network/connection errors so the renderer can show the
    // "cannot connect to update server — download installer from website"
    // message instead of a raw error string.
    const isNetworkError = /ECONNREFUSED|ETIMEDOUT|ENOTFOUND|ECONNRESET|EHOSTUNREACH|ENETUNREACH|net::ERR|getaddrinfo|fetch failed|network|timeout|hang up|socket/i.test(msg);
    send('update-status', {
      status: 'error',
      errorType: isNetworkError ? 'network' : 'generic',
      message: msg,
    });
  });

  // IPC handlers
  ipcMain.handle('check-for-updates', () => {
    if (!app.isPackaged) {
      send('update-status', { status: 'error', message: 'Updates are only available in the installed version.' });
      return;
    }
    if (!autoUpdater) {
      send('update-status', { status: 'error', message: 'Updater not available.' });
      return;
    }
    autoUpdater.checkForUpdates();
  });

  ipcMain.on('install-update', () => {
    if (autoUpdater) {
      autoUpdater.quitAndInstall();
    }
  });
}

/**
 * Send a message to the renderer if the window is alive.
 */
function send(channel, data) {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send(channel, data);
  }
}

module.exports = { initUpdater };
