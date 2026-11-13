package main

// 数据存储：JSON 文件 + 目录树（无数据库依赖，整个 data/ 目录可直接备份）。
//
//	data/
//	  users.json          用户表（随机盐 + PBKDF2-HMAC-SHA256 哈希）
//	  tokens.json         token → {uid, expires}（90 天滑动有效期）
//	  files/<uid>/
//	    config.json       该用户配置快照（白名单字段）
//	    animations/...    自定义角色 GIF / 音频（保留相对路径）
//	    live2d/...        用户 Live2D 模型（整目录树）
//	  releases/
//	    app.json          {version, notes, sha256, size, file}
//	    DutyOn-update-vX.Y.Z.zip
//
// manifest 不落盘：GET /api/sync/manifest 时实时遍历磁盘计算（永不漂移）。

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	quotaPerUser = int64(500 << 20) // 500MB / 用户
	maxFileSize  = int64(50 << 20)  // 50MB / 单文件
	tokenTTL     = 90 * 24 * time.Hour
)

type User struct {
	ID         string    `json:"id"`
	Name       string    `json:"name"`
	Salt       string    `json:"salt"` // hex(16B)
	Hash       string    `json:"hash"` // hex(32B) PBKDF2-HMAC-SHA256
	Created    time.Time `json:"created"`
	QuotaBytes int64     `json:"quotaBytes"`
}

type tokenInfo struct {
	UID     string    `json:"uid"`
	Expires time.Time `json:"expires"`
}

type manifestEntry struct {
	Path   string `json:"path"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

type Store struct {
	mu     sync.Mutex
	dir    string
	users  []User
	tokens map[string]tokenInfo
}

func OpenStore(dir string) (*Store, error) {
	s := &Store{dir: dir, tokens: map[string]tokenInfo{}}
	for _, d := range []string{dir, s.filesDir(), s.releasesDir()} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return nil, err
		}
	}
	if err := s.loadUsers(); err != nil {
		return nil, err
	}
	return s, s.loadTokens()
}

func (s *Store) filesDir() string    { return filepath.Join(s.dir, "files") }
func (s *Store) releasesDir() string { return filepath.Join(s.dir, "releases") }
func (s *Store) userDir(uid string) string {
	return filepath.Join(s.filesDir(), uid)
}
func (s *Store) userConfigPath(uid string) string {
	return filepath.Join(s.userDir(uid), "config.json")
}

// ---- users.json ----

func (s *Store) loadUsers() error {
	b, err := os.ReadFile(filepath.Join(s.dir, "users.json"))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var wrapper struct {
		Users []User `json:"users"`
	}
	if err := json.Unmarshal(b, &wrapper); err != nil {
		return err
	}
	s.users = wrapper.Users
	return nil
}

func (s *Store) saveUsersLocked() error {
	b, err := json.MarshalIndent(struct {
		Users []User `json:"users"`
	}{s.users}, "", "  ")
	if err != nil {
		return err
	}
	return atomicWrite(filepath.Join(s.dir, "users.json"), b)
}

// ---- tokens.json ----

func (s *Store) loadTokens() error {
	b, err := os.ReadFile(filepath.Join(s.dir, "tokens.json"))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	err = json.Unmarshal(b, &s.tokens)
	if s.tokens == nil {
		s.tokens = map[string]tokenInfo{}
	}
	return err
}

func (s *Store) saveTokensLocked() error {
	b, err := json.MarshalIndent(s.tokens, "", "  ")
	if err != nil {
		return err
	}
	return atomicWrite(filepath.Join(s.dir, "tokens.json"), b)
}

// ---- 用户 / token ----

func (s *Store) CreateUser(name, saltHex, hashHex string) (User, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, u := range s.users {
		if u.Name == name {
			return User{}, os.ErrExist
		}
	}
	id, err := randHex(16)
	if err != nil {
		return User{}, err
	}
	u := User{
		ID:         id,
		Name:       name,
		Salt:       saltHex,
		Hash:       hashHex,
		Created:    time.Now().UTC(),
		QuotaBytes: quotaPerUser,
	}
	s.users = append(s.users, u)
	if err := s.saveUsersLocked(); err != nil {
		s.users = s.users[:len(s.users)-1]
		return User{}, err
	}
	return u, nil
}

func (s *Store) FindUser(name string) (User, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, u := range s.users {
		if u.Name == name {
			return u, true
		}
	}
	return User{}, false
}

func (s *Store) FindUserByID(uid string) (User, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, u := range s.users {
		if u.ID == uid {
			return u, true
		}
	}
	return User{}, false
}

func (s *Store) IssueToken(uid string) (string, error) {
	tok, err := randHex(32)
	if err != nil {
		return "", err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.tokens[tok] = tokenInfo{UID: uid, Expires: time.Now().Add(tokenTTL)}
	if err := s.saveTokensLocked(); err != nil {
		delete(s.tokens, tok)
		return "", err
	}
	return tok, nil
}

// CheckToken 校验并滑动续期（有效期 90 天；续期写盘按小时节流）。
func (s *Store) CheckToken(token string) (string, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	info, ok := s.tokens[token]
	if !ok || time.Now().After(info.Expires) {
		if ok {
			delete(s.tokens, token)
			_ = s.saveTokensLocked()
		}
		return "", false
	}
	if time.Until(info.Expires) < tokenTTL-time.Hour {
		info.Expires = time.Now().Add(tokenTTL)
		s.tokens[token] = info
		_ = s.saveTokensLocked()
	}
	return info.UID, true
}

func (s *Store) RevokeToken(token string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.tokens[token]; ok {
		old := s.tokens[token]
		delete(s.tokens, token)
		if err := s.saveTokensLocked(); err != nil {
			s.tokens[token] = old
			return err
		}
	}
	return nil
}

// ---- 同步文件树 ----

// validSyncPath 白名单校验：仅 animations/ 与 live2d/ 前缀、无 `..`、
// 无反斜杠/绝对路径、必须指向具体文件（非目录本身）。
func validSyncPath(rel string) (string, bool) {
	if rel == "" || len(rel) > 512 {
		return "", false
	}
	if strings.ContainsAny(rel, "\\\x00:") || strings.Contains(rel, "..") {
		return "", false
	}
	if !strings.HasPrefix(rel, "animations/") &&
		!strings.HasPrefix(rel, "live2d/") {
		return "", false
	}
	for _, part := range strings.Split(rel, "/") {
		if part == "" || strings.HasSuffix(part, ".") || strings.HasSuffix(part, " ") || strings.ContainsAny(part, "<>\"|?*") {
			return "", false
		}
		for _, r := range part {
			if r < 32 || r == 127 {
				return "", false
			}
		}
		base := strings.ToUpper(strings.SplitN(part, ".", 2)[0])
		if base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || (len(base) == 4 && (strings.HasPrefix(base, "COM") || strings.HasPrefix(base, "LPT")) && base[3] >= '0' && base[3] <= '9') {
			return "", false
		}
	}
	cleaned := filepath.ToSlash(filepath.Clean(filepath.FromSlash(rel)))
	if cleaned != rel {
		return "", false // 规整后不一致（如 a//b、a/./b）→ 拒绝
	}
	if rel == "animations/" || rel == "live2d/" {
		return "", false
	}
	return rel, true
}

// userUsage 遍历该用户 animations/ + live2d/ 统计已用字节数（配额依据）。
func (s *Store) Usage(uid string) (int64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.userUsage(uid)
}

func (s *Store) userUsage(uid string) (int64, error) {
	var total int64
	for _, top := range []string{"animations", "live2d"} {
		root := filepath.Join(s.userDir(uid), top)
		err := filepath.WalkDir(root, func(_ string, d fs.DirEntry, err error) error {
			if err != nil {
				if errors.Is(err, os.ErrNotExist) {
					return nil
				}
				return err
			}
			if d != nil && !d.IsDir() {
				if info, err := d.Info(); err == nil {
					total += info.Size()
				}
			}
			return nil
		})
		if err != nil {
			return 0, err
		}
	}
	return total, nil
}

// Manifest 实时计算清单：[{path, size, sha256}]，按 path 排序。
func (s *Store) Manifest(uid string) ([]manifestEntry, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []manifestEntry
	for _, top := range []string{"animations", "live2d"} {
		root := filepath.Join(s.userDir(uid), top)
		err := filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
			if err != nil {
				if errors.Is(err, os.ErrNotExist) {
					return nil
				}
				return err
			}
			if d == nil || d.IsDir() {
				return nil
			}
			info, err := d.Info()
			if err != nil {
				return err
			}
			rel, err := filepath.Rel(s.userDir(uid), p)
			if err != nil {
				return err
			}
			sum, err := sha256File(p)
			if err != nil {
				return err
			}
			out = append(out, manifestEntry{
				Path:   filepath.ToSlash(rel),
				Size:   info.Size(),
				SHA256: sum,
			})
			return nil
		})
		if err != nil {
			return nil, err
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out, nil
}

// WriteFile 配额校验 + 落盘（替换已有文件时先扣除旧占用）。
func (s *Store) WriteFile(uid, rel string, data []byte) error {
	return s.WriteFileConditional(uid, rel, data, "", false)
}

func (s *Store) WriteFileConditional(uid, rel string, data []byte, match string, absent bool) error {
	if _, ok := validSyncPath(rel); !ok {
		return errors.New("invalid path")
	}
	if int64(len(data)) > maxFileSize {
		return errTooLarge
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	target := filepath.Join(s.userDir(uid), filepath.FromSlash(rel))
	if absent {
		if _, err := os.Stat(target); err == nil {
			return errConflict
		} else if !errors.Is(err, os.ErrNotExist) {
			return err
		}
	}
	if match != "" {
		sum, err := sha256File(target)
		if err != nil || sum != match {
			return errConflict
		}
	}
	var oldSize int64
	if st, err := os.Stat(target); err == nil {
		oldSize = st.Size()
	}
	usage, err := s.userUsage(uid)
	if err != nil {
		return err
	}
	if usage-oldSize+int64(len(data)) > quotaPerUser {
		return errQuota
	}
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return err
	}
	return atomicWrite(target, data)
}

func (s *Store) ReadFile(uid, rel string) ([]byte, error) {
	if _, ok := validSyncPath(rel); !ok {
		return nil, errors.New("invalid path")
	}
	return os.ReadFile(filepath.Join(s.userDir(uid), filepath.FromSlash(rel)))
}

func (s *Store) DeleteFile(uid, rel string) error {
	if _, ok := validSyncPath(rel); !ok {
		return errors.New("invalid path")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	err := os.Remove(filepath.Join(s.userDir(uid), filepath.FromSlash(rel)))
	if errors.Is(err, os.ErrNotExist) {
		return nil // 幂等：云端已无此文件也算删除成功
	}
	return err
}

var syncFields = []string{"flipHorizontal", "miniMode", "language", "activeCharacterId", "stateMotions", "customCharacters", "deviceMode", "frameSource", "clockColor", "characterColors", "deviceBrightness", "deviceVolume", "stateAudio", "soundMute", "stateAudioMuted", "screenRotation"}

// ---- 配置快照 ----

// 配置快照的版本号：保存内容的 SHA-256 前 32 位；还没有快照时为 "0"。
// 网页和桌宠都可能改设置，写入时带上自己读到的版本号（If-Match），
// 版本对不上说明别处已经改过，拒绝覆盖。
func configRev(b []byte) string {
	if len(b) == 0 {
		return "0"
	}
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:16])
}

var errConfigConflict = errors.New("config changed elsewhere")

// SaveConfig 保存白名单字段。
//
// ifMatch 非空（网页、新版桌宠）：必须等于当前版本号，否则返回
// errConfigConflict 和当前版本号。
//
// ifMatch 为空（2.0.16 及更早的桌宠，每分钟无条件上传本机设置）：如果云端
// 这一版是带版本号写入的——也就是网页或新版桌宠改过——而上传内容又与它
// 不同，同样拒绝。否则旧版桌宠会在一分钟内把网页上的修改悄悄盖掉。旧版桌宠
// 此时提示同步失败，用户"从云端恢复"后内容一致，上传即恢复正常。
func (s *Store) SaveConfig(uid string, cfg map[string]any, ifMatch string) (string, error) {
	filtered := make(map[string]any)
	for _, key := range syncFields {
		if v, ok := cfg[key]; ok {
			filtered[key] = v
		}
	}
	b, err := json.MarshalIndent(filtered, "", "  ")
	if err != nil {
		return "", err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	cur, _ := os.ReadFile(s.userConfigPath(uid))
	curRev := configRev(cur)
	guardPath := s.userConfigPath(uid) + ".guard"
	if ifMatch != "" {
		if curRev != ifMatch {
			return curRev, errConfigConflict
		}
	} else if guard, _ := os.ReadFile(guardPath); string(guard) == curRev && !bytes.Equal(b, cur) {
		return curRev, errConfigConflict
	}
	if err := os.MkdirAll(s.userDir(uid), 0o755); err != nil {
		return "", err
	}
	if err := atomicWrite(s.userConfigPath(uid), b); err != nil {
		return "", err
	}
	rev := configRev(b)
	if ifMatch != "" {
		// 记下"这一版是带版本号写入的"，供上面的无条件写入判断
		if err := atomicWrite(guardPath, []byte(rev)); err != nil {
			return "", err
		}
	}
	return rev, nil
}

func (s *Store) LoadConfig(uid string) (map[string]any, string, error) {
	b, err := os.ReadFile(s.userConfigPath(uid))
	if errors.Is(err, os.ErrNotExist) {
		return make(map[string]any), "0", nil
	}
	if err != nil {
		return nil, "", err
	}
	cfg := make(map[string]any)
	if err := json.Unmarshal(b, &cfg); err != nil {
		return make(map[string]any), configRev(b), nil // 损坏快照当空处理
	}
	return cfg, configRev(b), nil
}

// ---- 小工具 ----

var (
	errTooLarge = errors.New("file too large")
	errQuota    = errors.New("quota exceeded")
	errConflict = errors.New("file changed")
)

func randHex(n int) (string, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

// atomicWrite 临时文件 + 原子改名（避免写一半被读到）。
func atomicWrite(path string, data []byte) error {
	tmp, err := os.CreateTemp(filepath.Dir(path), ".dutyon-*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if _, err = tmp.Write(data); err != nil {
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
	return os.Rename(name, path)
}
