package main

// 应用内升级：发布子命令 + 版本查询 + 升级包下载。
//
// 发布：dutyon-cloud --publish <DutyOn-update-vX.Y.Z.zip> [--notes "..."]
//   解析文件名版本号 → 计算 sha256/size → 拷入 data/releases/ → 写 app.json
// 查询：GET /api/app/version
//   无发布 → 404（客户端据此显示「已是最新」）
//   有发布 → 200 {version, notes, sha256, size}（新旧比较由客户端做）
// 下载：GET /api/app/download → zip 字节流（客户端本地 sha256 校验）

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
)

var releaseNameRe = regexp.MustCompile(`DutyOn-update-v(\d+(?:\.\d+){1,3})\.zip$`)

type releaseInfo struct {
	Version string `json:"version"`
	Notes   string `json:"notes"`
	SHA256  string `json:"sha256"`
	Size    int64  `json:"size"`
	File    string `json:"file"`
}

func sha256File(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func (a *App) releaseMetaPath() string {
	return filepath.Join(a.store.dir, "releases", "app.json")
}

// publish 发布升级包：校验文件名 → 算哈希 → 拷入 releases/ → 写 app.json。
func (a *App) publish(zipPath, notes string) error {
	m := releaseNameRe.FindStringSubmatch(filepath.Base(zipPath))
	if m == nil {
		return fmt.Errorf("文件名需形如 DutyOn-update-vX.Y.Z.zip，实际: %s",
			filepath.Base(zipPath))
	}
	src, err := os.Open(zipPath)
	if err != nil {
		return err
	}
	defer src.Close()

	rel := releaseInfo{Version: m[1], Notes: notes}
	st, err := src.Stat()
	if err != nil {
		return err
	}
	rel.Size = st.Size()

	h := sha256.New()
	if _, err := io.Copy(h, src); err != nil {
		return err
	}
	rel.SHA256 = hex.EncodeToString(h.Sum(nil))
	if _, err := src.Seek(0, io.SeekStart); err != nil {
		return err
	}

	dest := filepath.Join(a.store.releasesDir(), filepath.Base(zipPath))
	out, err := os.Create(dest + ".tmp")
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, src); err != nil {
		out.Close()
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	if err := os.Rename(dest+".tmp", dest); err != nil {
		return err
	}
	rel.File = filepath.Base(dest)

	meta, err := json.MarshalIndent(rel, "", "  ")
	if err != nil {
		return err
	}
	return atomicWrite(a.releaseMetaPath(), meta)
}

func (a *App) loadRelease() (releaseInfo, bool) {
	b, err := os.ReadFile(a.releaseMetaPath())
	if err != nil {
		return releaseInfo{}, false
	}
	var rel releaseInfo
	if err := json.Unmarshal(b, &rel); err != nil || rel.Version == "" {
		return releaseInfo{}, false
	}
	return rel, true
}

func (a *App) handleVersion(w http.ResponseWriter, _ *http.Request) {
	rel, ok := a.loadRelease()
	if !ok {
		// 客户端契约：404 = 从未发布 → 「已是最新」
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no release"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"version": rel.Version,
		"notes":   rel.Notes,
		"sha256":  rel.SHA256,
		"size":    rel.Size,
	})
}

func (a *App) handleDownload(w http.ResponseWriter, r *http.Request) {
	rel, ok := a.loadRelease()
	if !ok {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no release"})
		return
	}
	w.Header().Set("Content-Type", "application/zip")
	http.ServeFile(w, r, filepath.Join(a.store.releasesDir(), rel.File))
}
