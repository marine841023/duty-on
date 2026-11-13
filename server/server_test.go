package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func testApp(t *testing.T) (*App, http.Handler) {
	t.Helper()
	s, err := OpenStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	a := &App{store: s, limiter: newLoginLimiter()}
	return a, a.Handler()
}
func call(h http.Handler, method, path, token string, body []byte) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	return w
}
func account(t *testing.T, h http.Handler, name string) string {
	t.Helper()
	w := call(h, "POST", "/api/auth/register", "", []byte(fmt.Sprintf(`{"username":%q,"password":"test-password"}`, name)))
	if w.Code != 201 {
		t.Fatalf("register: %d %s", w.Code, w.Body)
	}
	var data map[string]string
	if err := json.Unmarshal(w.Body.Bytes(), &data); err != nil {
		t.Fatal(err)
	}
	return data["token"]
}
func TestAccountPersistenceAndIsolation(t *testing.T) {
	a, h := testApp(t)
	alice := account(t, h, "alice")
	bob := account(t, h, "bob")
	if w := call(h, "GET", "/api/auth/register", "", nil); w.Code != 405 {
		t.Fatal(w.Code)
	}
	if w := call(h, "POST", "/api/auth/login", "", []byte(`{"username":"alice","password":"wrong"}`)); w.Code != 401 {
		t.Fatal(w.Code)
	}
	if w := call(h, "POST", "/api/auth/login", "", []byte(`{"username":"alice","password":"test-password"}`)); w.Code != 200 {
		t.Fatal(w.Code)
	}
	path := "/api/sync/file?path=animations/pet.gif"
	for _, data := range []string{"first", "replacement"} {
		if w := call(h, "PUT", path, alice, []byte(data)); w.Code != 200 {
			t.Fatal(w.Code, w.Body)
		}
	}
	if w := call(h, "GET", path, bob, nil); w.Code != 404 {
		t.Fatal("cross-account read", w.Code)
	}
	if w := call(h, "GET", path, "", nil); w.Code != 401 {
		t.Fatal(w.Code)
	}
	if w := call(h, "PUT", "/api/sync/config", alice, []byte(`{"language":"zh","cloudToken":"secret","pairToken":"secret"}`)); w.Code != 200 {
		t.Fatal(w.Code)
	}
	w := call(h, "GET", "/api/sync/config", alice, nil)
	if bytes.Contains(w.Body.Bytes(), []byte("secret")) || !bytes.Contains(w.Body.Bytes(), []byte("language")) {
		t.Fatal(w.Body)
	}
	// 设置带版本号：带旧版本号写入被拒绝，带当前版本号写入成功并得到新版本号
	var got struct {
		Rev string `json:"rev"`
	}
	json.Unmarshal(w.Body.Bytes(), &got)
	if len(got.Rev) != 32 {
		t.Fatal("config must carry a revision", w.Body)
	}
	put := func(rev, body string) *httptest.ResponseRecorder {
		req := httptest.NewRequest("PUT", "/api/sync/config", bytes.NewReader([]byte(body)))
		req.Header.Set("Authorization", "Bearer "+alice)
		req.Header.Set("If-Match", "\""+rev+"\"")
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		return rec
	}
	if rec := put("0", `{"language":"en"}`); rec.Code != 412 || !bytes.Contains(rec.Body.Bytes(), []byte(got.Rev)) {
		t.Fatal("stale revision must be rejected with the current one", rec.Code, rec.Body)
	}
	rec := put(got.Rev, `{"language":"en","customCharacters":[{"id":"char_1","name":"测试"}]}`)
	if rec.Code != 200 || bytes.Contains(rec.Body.Bytes(), []byte(got.Rev)) {
		t.Fatal("matching revision must be accepted and return a new one", rec.Code, rec.Body)
	}
	if rec := put(got.Rev, `{"language":"ja"}`); rec.Code != 412 {
		t.Fatal("old revision must not win after an update", rec.Code)
	}
	// 旧版桌宠不带版本号、每分钟上传本机设置：云端被网页改过之后，内容不同的
	// 无条件上传必须被拒绝（否则网页的修改一分钟内就被盖掉）；内容相同照常通过
	if w := call(h, "PUT", "/api/sync/config", alice, []byte(`{"language":"zh"}`)); w.Code != 412 {
		t.Fatal("unconditional write must not clobber a web edit", w.Code)
	}
	if w := call(h, "PUT", "/api/sync/config", alice, []byte(`{"customCharacters":[{"id":"char_1","name":"测试"}],"language":"en"}`)); w.Code != 200 {
		t.Fatal("unconditional write of identical content must pass", w.Code, w.Body)
	}
	// 从没用过网页的账号：旧版桌宠的无条件上传不受影响
	if w := call(h, "PUT", "/api/sync/config", bob, []byte(`{"language":"zh"}`)); w.Code != 200 {
		t.Fatal(w.Code)
	}
	if w := call(h, "PUT", "/api/sync/config", bob, []byte(`{"language":"en"}`)); w.Code != 200 {
		t.Fatal("accounts never edited on the web keep last-write-wins", w.Code)
	}
	s, err := OpenStore(a.store.dir)
	if err != nil {
		t.Fatal(err)
	}
	restarted := (&App{store: s, limiter: newLoginLimiter()}).Handler()
	if w := call(restarted, "GET", path, alice, nil); w.Code != 200 || w.Body.String() != "replacement" {
		t.Fatal(w.Code, w.Body)
	}
	if w := call(restarted, "POST", "/api/auth/logout", alice, nil); w.Code != 200 {
		t.Fatal(w.Code)
	}
	s, err = OpenStore(a.store.dir)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := s.CheckToken(alice); ok {
		t.Fatal("logout did not persist")
	}
}
func TestValidationAndConcurrentManifest(t *testing.T) {
	a, h := testApp(t)
	token := account(t, h, "tester")
	for _, body := range []string{`null`, `[]`, `{"username":"tester","password":"test-password"} {}`, `{"username":4}`} {
		if w := call(h, "POST", "/api/auth/login", "", []byte(body)); w.Code != 400 {
			t.Fatalf("invalid json %q: %d", body, w.Code)
		}
	}
	for _, path := range []string{"../x", "animations/../x", "animations/a:b", "animations/a\\b", "animations//a", "animations/CON.gif", "live2d/a./x", "animations/", "live2d/x\x00"} {
		if _, ok := validSyncPath(path); ok {
			t.Errorf("accepted %q", path)
		}
	}
	if err := a.store.WriteFile("test", "animations/a", make([]byte, maxFileSize+1)); err != errTooLarge {
		t.Fatal(err)
	}
	// Sparse existing file exercises quota enforcement without allocating 500 MiB.
	dir := filepath.Join(a.store.userDir("quota"), "animations")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	f, err := os.Create(filepath.Join(dir, "full"))
	if err != nil {
		t.Fatal(err)
	}
	err = f.Truncate(quotaPerUser)
	f.Close()
	if err != nil {
		t.Fatal(err)
	}
	if err := a.store.WriteFile("quota", "animations/extra", []byte("x")); err != errQuota {
		t.Fatal(err)
	}
	uid, _ := a.store.CheckToken(token)
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			for j := 0; j < 5; j++ {
				if err := a.store.WriteFile(uid, fmt.Sprintf("animations/%d", i), []byte("test")); err != nil {
					t.Error(err)
				}
				entries, err := a.store.Manifest(uid)
				if err != nil {
					t.Error(err)
				}
				for _, e := range entries {
					if bytes.Contains([]byte(e.Path), []byte(".dutyon-")) {
						t.Error("temporary file leaked")
					}
				}
			}
		}(i)
	}
	wg.Wait()
	if err := a.store.WriteFileConditional(uid, "animations/0", []byte("no"), "", true); err != errConflict {
		t.Fatal(err)
	}
	if err := a.store.WriteFileConditional(uid, "animations/0", []byte("no"), fmt.Sprintf("%064d", 0), false); err != errConflict {
		t.Fatal(err)
	}
}
func makeRelease(t *testing.T, dir, version, kind string) string {
	t.Helper()
	name := "DutyOn-update-v" + version + ".zip"
	entry := "dutyon-pet.exe"
	if kind == "bundle" {
		name = "DutyOn-v" + version + ".zip"
		entry = "DutyOn_" + version + "_x64-setup.exe"
	}
	path := filepath.Join(dir, name)
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	z := zip.NewWriter(f)
	w, err := z.Create(entry)
	if err != nil {
		t.Fatal(err)
	}
	_, _ = w.Write([]byte("MZ-" + version))
	if err = z.Close(); err != nil {
		t.Fatal(err)
	}
	f.Close()
	return path
}
func TestImmutableReleasesAndWeb(t *testing.T) {
	a, h := testApp(t)
	dir := t.TempDir()
	if w := call(h, "GET", "/api/app/version", "", nil); w.Code != 404 {
		t.Fatal(w.Code)
	}
	first := makeRelease(t, dir, "2.0.15", "update")
	if err := a.publish(first, "first"); err != nil {
		t.Fatal(err)
	}
	old, _ := a.loadRelease()
	before, err := os.ReadFile(first)
	if err != nil {
		t.Fatal(err)
	}
	if err := a.publish(makeRelease(t, dir, "2.0.16", "update"), "second"); err != nil {
		t.Fatal(err)
	}
	w := call(h, "GET", "/api/app/download?sha256="+old.SHA256, "", nil)
	if w.Code != 200 || !bytes.Equal(w.Body.Bytes(), before) {
		t.Fatal("pinned artifact changed", w.Code)
	}
	if err := a.publishArtifact(makeRelease(t, dir, "2.0.16", "bundle"), "", "bundle"); err != nil {
		t.Fatal(err)
	}
	installer := filepath.Join(dir, "DutyOn_2.0.16_x64-setup.exe")
	os.WriteFile(installer, []byte("MZ-test"), 0600)
	if err := a.publishArtifact(installer, "", "installer"); err != nil {
		t.Fatal(err)
	}
	w = call(h, "GET", "/api/app/releases", "", nil)
	var releases map[string]releaseInfo
	if err := json.Unmarshal(w.Body.Bytes(), &releases); err != nil || len(releases) != 3 {
		t.Fatal(err, w.Body)
	}
	for kind, rel := range releases {
		if w := call(h, "GET", rel.DownloadURL, "", nil); w.Code != 200 {
			t.Fatal(kind, w.Code)
		}
	}
	bad := filepath.Join(dir, "DutyOn-update-v9.9.9.zip")
	os.WriteFile(bad, []byte("broken"), 0600)
	if err := a.publish(bad, ""); err == nil {
		t.Fatal("accepted invalid zip")
	}
	for _, path := range []string{"/", "/app.js", "/style.css", "/img/icon.png", "/img/nipsilon.png", "/healthz"} {
		if w := call(h, "GET", path, "", nil); w.Code != 200 || w.Header().Get("X-Content-Type-Options") != "nosniff" {
			t.Fatal(path, w.Code)
		}
	}
	// 网页只发 web/ 下确实存在的文件：目录、不存在的路径、源码文件都是 404
	for _, path := range []string{"/img/", "/img", "/nope.js", "/store.go", "/web/index.html"} {
		if w := call(h, "GET", path, "", nil); w.Code != 404 {
			t.Fatal(path, w.Code)
		}
	}
	if w := call(h, "GET", "/", "", nil); !strings.Contains(w.Header().Get("Content-Security-Policy"), "default-src 'none'") ||
		!strings.Contains(w.Body.String(), "云空间") {
		t.Fatal("home page must carry CSP and content")
	}
	if w := call(h, "GET", "/data/users.json", "", nil); w.Code != 404 {
		t.Fatal(w.Code)
	}
}

func TestNullTokenStore(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "tokens.json"), []byte("null"), 0600)
	s, err := OpenStore(dir)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = s.IssueToken("test"); err != nil {
		t.Fatal(err)
	}
}
