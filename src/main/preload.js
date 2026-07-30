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
  dragWindow: (deltaX, deltaY) => ipcRenderer.send('drag-window', deltaX, deltaY),
  bringToFront: (projectPath) => ipcRenderer.send('bring-to-front', projectPath),
  flashAttention: () => ipcRenderer.send('flash-attention'),
  quit: () => ipcRenderer.send('quit'),
});
