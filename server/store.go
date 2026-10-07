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
	"crypto/rand"
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
	return json.Unmarshal(b, &s.tokens)
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
	return u, s.saveUsersLocked()
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
		return "", err
	}
	return tok, nil
}

// CheckToken 校验并滑动续期（每次使用重置 90 天；写盘按分钟节流）。
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
	info.Expires = time.Now().Add(tokenTTL)
	s.tokens[token] = info
	_ = s.saveTokensLocked() // 个人规模：直接落盘，失败不影响本次请求
	return info.UID, true
}

func (s *Store) RevokeToken(token string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.tokens[token]; ok {
		delete(s.tokens, token)
		_ = s.saveTokensLocked()
	}
}

// ---- 同步文件树 ----

// validSyncPath 白名单校验：仅 animations/ 与 live2d/ 前缀、无 `..`、
// 无反斜杠/绝对路径、必须指向具体文件（非目录本身）。
func validSyncPath(rel string) (string, bool) {
	rel = strings.Trim(rel, "/")
	if rel == "" || len(rel) > 512 {
		return "", false
	}
	if strings.ContainsAny(rel, "\\\x00") || strings.Contains(rel, "..") {
		return "", false
	}
	if !strings.HasPrefix(rel, "animations/") &&
		!strings.HasPrefix(rel, "live2d/") {
		return "", false
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
	if int64(len(data)) > maxFileSize {
		return errTooLarge
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	target := filepath.Join(s.userDir(uid), filepath.FromSlash(rel))
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
	return os.ReadFile(filepath.Join(s.userDir(uid), filepath.FromSlash(rel)))
}

func (s *Store) DeleteFile(uid, rel string) error {
	err := os.Remove(filepath.Join(s.userDir(uid), filepath.FromSlash(rel)))
	if errors.Is(err, os.ErrNotExist) {
		return nil // 幂等：云端已无此文件也算删除成功
	}
	return err
}

// ---- 配置快照 ----

func (s *Store) SaveConfig(uid string, cfg map[string]interface{}) error {
	b, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := os.MkdirAll(s.userDir(uid), 0o755); err != nil {
		return err
	}
	return atomicWrite(s.userConfigPath(uid), b)
}

func (s *Store) LoadConfig(uid string) (map[string]interface{}, error) {
	b, err := os.ReadFile(s.userConfigPath(uid))
	if errors.Is(err, os.ErrNotExist) {
		return map[string]interface{}{}, nil
	}
	if err != nil {
		return nil, err
	}
	cfg := map[string]interface{}{}
	if err := json.Unmarshal(b, &cfg); err != nil {
		return map[string]interface{}{}, nil // 损坏快照当空处理
	}
	return cfg, nil
}

// ---- 小工具 ----

var (
	errTooLarge = errors.New("file too large")
	errQuota    = errors.New("quota exceeded")
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
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}
