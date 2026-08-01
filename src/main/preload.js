/**
 * Preload script - bridges Electron IPC to the renderer process.
 * Exposes a safe `petAPI` object via contextBridge.
 */

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('petAPI', {
  onStateUpdate: (callback) => ipcRenderer.on('state-update', (_, data) => callback(data)),
  onAlert: (callback) => ipcRenderer.on('alert', (_, data) => callback(data)),
  installHooks: () => ipcRenderer.invoke('install-hooks'),
  isHooksInstalled: () => ipcRenderer.invoke('is-hooks-installed'),
  getModels: () => ipcRenderer.invoke('get-models'),
  switchModel: (modelUrl) => ipcRenderer.send('switch-model', modelUrl),
  getStateMotions: () => ipcRenderer.invoke('get-state-motions'),
  setStateMotions: (motions) => ipcRenderer.send('set-state-motions', motions),
  getAppearance: () => ipcRenderer.invoke('get-appearance'),
  setFlipHorizontal: (enabled) => ipcRenderer.send('set-flip-horizontal', enabled),
  getLanguage: () => ipcRenderer.invoke('get-language'),
  setLanguage: (lang) => ipcRenderer.send('set-language', lang),
  getAutoLaunch: () => ipcRenderer.invoke('get-auto-launch'),
  setAutoLaunch: (enabled) => ipcRenderer.send('set-auto-launch', enabled),
  checkForUpdates: () => ipcRenderer.invoke('check-for-updates'),
  installUpdate: () => ipcRenderer.send('install-update'),
  onUpdateStatus: (callback) => ipcRenderer.on('update-status', (_, data) => callback(data)),
  testAlert: () => ipcRenderer.send('test-alert'),
  dragWindow: (deltaX, deltaY) => ipcRenderer.send('drag-window', deltaX, deltaY),
  setClickThrough: (ignore) => ipcRenderer.send('set-click-through', ignore),
  bringToFront: (projectPath) => ipcRenderer.send('bring-to-front', projectPath),
  flashAttention: () => ipcRenderer.send('flash-attention'),
  quit: () => ipcRenderer.send('quit'),
  uninstallApp: () => ipcRenderer.send('uninstall-app'),
});
