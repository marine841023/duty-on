package main

// Release metadata is swapped only after the immutable, content-addressed artifact is durable.
import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

var releaseNames = map[string]*regexp.Regexp{
	"update":    regexp.MustCompile(`^DutyOn-update-v(\d+(?:\.\d+){1,3})\.zip$`),
	"bundle":    regexp.MustCompile(`^DutyOn-v(\d+(?:\.\d+){1,3})\.zip$`),
	"installer": regexp.MustCompile(`^DutyOn_(\d+(?:\.\d+){1,3})_x64-setup\.exe$`),
}
var digestRE = regexp.MustCompile(`^[a-f0-9]{64}$`)

type releaseInfo struct {
	Version     string `json:"version"`
	Notes       string `json:"notes"`
	SHA256      string `json:"sha256"`
	Size        int64  `json:"size"`
	File        string `json:"file"`
	Kind        string `json:"kind,omitempty"`
	DownloadURL string `json:"downloadUrl,omitempty"`
}

func sha256File(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err = io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
func (a *App) releaseMetaPath() string { return a.releaseMetaKind("update") }
func (a *App) releaseMetaKind(kind string) string {
	name := kind + ".json"
	if kind == "update" {
		name = "app.json"
	}
	return filepath.Join(a.store.releasesDir(), name)
}
func (a *App) publish(path, notes string) error { return a.publishArtifact(path, notes, "update") }
func (a *App) publishArtifact(path, notes, kind string) error {
	re, ok := releaseNames[kind]
	if !ok {
		return fmt.Errorf("unknown release kind %q", kind)
	}
	name := filepath.Base(path)
	m := re.FindStringSubmatch(name)
	if m == nil {
		return fmt.Errorf("invalid %s filename: %s", kind, name)
	}
	// Reject a mislabeled or truncated archive before making it the current release.
	if kind != "installer" {
		z, err := zip.OpenReader(path)
		if err != nil {
			return err
		}
		defer z.Close()
		hasExe := false
		for _, f := range z.File {
			p := strings.ReplaceAll(f.Name, "\\", "/")
			if strings.HasPrefix(p, "/") || strings.Contains(p, ":") {
				return fmt.Errorf("unsafe zip path")
			}
			for _, part := range strings.Split(p, "/") {
				if part == ".." {
					return fmt.Errorf("unsafe zip path")
				}
			}
			if (kind == "update" && p == "dutyon-pet.exe") || (kind == "bundle" && p == "DutyOn_"+m[1]+"_x64-setup.exe") {
				hasExe = true
			}
		}
		if !hasExe {
			return fmt.Errorf("zip does not contain the expected executable at its root")
		}
	}
	src, err := os.Open(path)
	if err != nil {
		return err
	}
	defer src.Close()
	if kind == "installer" {
		var magic [2]byte
		if _, err = io.ReadFull(src, magic[:]); err != nil || string(magic[:]) != "MZ" {
			return fmt.Errorf("not a Windows executable")
		}
		if _, err = src.Seek(0, 0); err != nil {
			return err
		}
	}
	tmp, err := os.CreateTemp(a.store.releasesDir(), ".publish-*")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	h := sha256.New()
	size, err := io.Copy(io.MultiWriter(tmp, h), src)
	if err != nil {
		tmp.Close()
		return err
	}
	if err = tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err = tmp.Close(); err != nil {
		return err
	}
	sum := hex.EncodeToString(h.Sum(nil))
	artifact := sum + filepath.Ext(name)
	dest := filepath.Join(a.store.releasesDir(), artifact)
	if _, err = os.Stat(dest); os.IsNotExist(err) {
		if err = os.Rename(tmp.Name(), dest); err != nil {
			return err
		}
	}
	rel := releaseInfo{Version: m[1], Notes: notes, SHA256: sum, Size: size, File: artifact, Kind: kind, DownloadURL: "/api/app/artifact?kind=" + kind + "&sha256=" + sum}
	b, err := json.MarshalIndent(rel, "", "  ")
	if err != nil {
		return err
	}
	if err = atomicWrite(filepath.Join(a.store.releasesDir(), sum+"-"+kind+".json"), b); err != nil {
		return err
	}
	return atomicWrite(a.releaseMetaKind(kind), b)
}
func (a *App) loadRelease() (releaseInfo, bool) { return a.loadReleaseKind("update") }
func (a *App) loadReleaseKind(kind string) (releaseInfo, bool) {
	if _, ok := releaseNames[kind]; !ok {
		return releaseInfo{}, false
	}
	return a.readRelease(a.releaseMetaKind(kind))
}
func (a *App) readRelease(path string) (releaseInfo, bool) {
	b, err := os.ReadFile(path)
	if err != nil {
		return releaseInfo{}, false
	}
	var rel releaseInfo
	if json.Unmarshal(b, &rel) != nil || rel.Version == "" || !digestRE.MatchString(rel.SHA256) || filepath.Base(rel.File) != rel.File {
		return releaseInfo{}, false
	}
	return rel, true
}
func (a *App) handleVersion(w http.ResponseWriter, r *http.Request) {
	rel, ok := a.loadRelease()
	if !ok {
		writeJSON(w, 404, map[string]string{"error": "no release"})
		return
	}
	writeJSON(w, 200, rel)
}
func (a *App) handleReleases(w http.ResponseWriter, r *http.Request) {
	out := map[string]releaseInfo{}
	for kind := range releaseNames {
		if rel, ok := a.loadReleaseKind(kind); ok {
			out[kind] = rel
		}
	}
	writeJSON(w, 200, out)
}
func (a *App) serveRelease(w http.ResponseWriter, r *http.Request, rel releaseInfo) {
	ext := filepath.Ext(rel.File)
	name := "DutyOn-v" + rel.Version + ext
	if rel.Kind == "update" {
		name = "DutyOn-update-v" + rel.Version + ext
	}
	if rel.Kind == "installer" {
		name = "DutyOn_" + rel.Version + "_x64-setup.exe"
	}
	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": name}))
	w.Header().Set("ETag", `"`+rel.SHA256+`"`)
	w.Header().Set("Content-Type", "application/octet-stream")
	http.ServeFile(w, r, filepath.Join(a.store.releasesDir(), rel.File))
}
func (a *App) handleArtifact(w http.ResponseWriter, r *http.Request) {
	sum := r.URL.Query().Get("sha256")
	if !digestRE.MatchString(sum) {
		writeJSON(w, 400, map[string]string{"error": "invalid digest"})
		return
	}
	kind := r.URL.Query().Get("kind")
	if kind == "" {
		kind = "update"
	}
	if _, ok := releaseNames[kind]; !ok {
		http.NotFound(w, r)
		return
	}
	rel, ok := a.readRelease(filepath.Join(a.store.releasesDir(), sum+"-"+kind+".json"))
	if !ok {
		http.NotFound(w, r)
		return
	}
	a.serveRelease(w, r, rel)
}
func (a *App) handleDownload(w http.ResponseWriter, r *http.Request) {
	// Optional digest pins downloads to the version the client actually checked.
	if r.URL.Query().Get("sha256") != "" {
		a.handleArtifact(w, r)
		return
	}
	rel, ok := a.loadReleaseKind("update")
	if !ok {
		writeJSON(w, 404, map[string]string{"error": "no release"})
		return
	}
	a.serveRelease(w, r, rel)
}
