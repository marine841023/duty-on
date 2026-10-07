package main

// 云同步：配置快照 GET/PUT + 文件清单 GET + 文件 GET/PUT/DELETE。
// 客户端契约（cloud_client.cpp）：
//   GET  /api/sync/config    → 200 {"config":{...}}（无数据也 200 + 空对象）
//   PUT  /api/sync/config    body=白名单字段对象 → 200
//   GET  /api/sync/manifest  → 200 [{path,size,sha256}]（JSON 数组）
//   GET  /api/sync/file?path=animations/x.gif → 200 binary / 404
//   PUT  /api/sync/file?path=... body=raw bytes → 200
//   DELETE /api/sync/file?path=... → 200（幂等）

import (
	"errors"
	"io"
	"net/http"
	"os"
)

func (a *App) handleConfigGet(w http.ResponseWriter, r *http.Request) {
	uid := r.Context().Value(uidKey{}).(string)
	cfg, err := a.store.LoadConfig(uid)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "读取配置失败"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"config": cfg})
}

func (a *App) handleConfigPut(w http.ResponseWriter, r *http.Request) {
	uid := r.Context().Value(uidKey{}).(string)
	var cfg map[string]interface{}
	if err := readJSON(w, r, &cfg, 1<<20); err != nil {
		return
	}
	if cfg == nil {
		cfg = map[string]interface{}{}
	}
	if err := a.store.SaveConfig(uid, cfg); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "保存配置失败"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (a *App) handleManifest(w http.ResponseWriter, r *http.Request) {
	uid := r.Context().Value(uidKey{}).(string)
	entries, err := a.store.Manifest(uid)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "清单计算失败"})
		return
	}
	if entries == nil {
		entries = []manifestEntry{} // JSON null → []
	}
	writeJSON(w, http.StatusOK, entries)
}

func (a *App) handleFileGet(w http.ResponseWriter, r *http.Request) {
	uid := r.Context().Value(uidKey{}).(string)
	rel, ok := validSyncPath(r.URL.Query().Get("path"))
	if !ok {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "非法路径"})
		return
	}
	data, err := a.store.ReadFile(uid, rel)
	if errors.Is(err, os.ErrNotExist) {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "文件不存在"})
		return
	}
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "读取失败"})
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
}

func (a *App) handleFilePut(w http.ResponseWriter, r *http.Request) {
	uid := r.Context().Value(uidKey{}).(string)
	rel, ok := validSyncPath(r.URL.Query().Get("path"))
	if !ok {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "非法路径"})
		return
	}
	// 限长读取：超 maxFileSize+1 即拒绝（兼容 Go 1.18 无 MaxBytesError）
	data, err := io.ReadAll(io.LimitReader(r.Body, maxFileSize+1))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "读取请求体失败"})
		return
	}
	err = a.store.WriteFile(uid, rel, data)
	switch {
	case err == nil:
		writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
	case errors.Is(err, errTooLarge):
		writeJSON(w, http.StatusRequestEntityTooLarge, map[string]string{
			"error": "单文件超过 50MB 限制"})
	case errors.Is(err, errQuota):
		writeJSON(w, http.StatusRequestEntityTooLarge, map[string]string{
			"error": "云端空间不足（500MB）"})
	default:
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "写入失败"})
	}
}

func (a *App) handleFileDelete(w http.ResponseWriter, r *http.Request) {
	uid := r.Context().Value(uidKey{}).(string)
	rel, ok := validSyncPath(r.URL.Query().Get("path"))
	if !ok {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "非法路径"})
		return
	}
	if err := a.store.DeleteFile(uid, rel); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "删除失败"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}
