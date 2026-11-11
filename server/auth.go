package main

// 账户：注册 / 登录 / 注销 / 用户信息。
// - 口令哈希 PBKDF2-HMAC-SHA256（100k 迭代，crypto/hmac + crypto/sha256
//   内联实现，无 x/crypto 依赖），16B 随机盐
// - token = 32B hex，90 天滑动有效期（store.CheckToken 续期）
// - 防爆破：同用户名连续 5 次登录失败锁 10s（内存计数，重启清零）

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"os"
	"regexp"
	"sync"
	"time"
)

const (
	pbkdf2Iters  = 100000
	pbkdf2KeyLen = 32
	saltBytes    = 16
)

var usernameRe = regexp.MustCompile(`^[A-Za-z0-9_-]{3,32}$`)

// pbkDF2 内联实现（RFC 2898，等价 golang.org/x/crypto/pbkdf2）。
func pbkDF2(password, salt []byte, iter, keyLen int) []byte {
	prf := hmac.New(sha256.New, password)
	hashLen := prf.Size()
	numBlocks := (keyLen + hashLen - 1) / hashLen
	dk := make([]byte, 0, numBlocks*hashLen)
	var blockBE [4]byte
	U := make([]byte, hashLen)
	for block := 1; block <= numBlocks; block++ {
		prf.Reset()
		prf.Write(salt)
		blockBE[0] = byte(block >> 24)
		blockBE[1] = byte(block >> 16)
		blockBE[2] = byte(block >> 8)
		blockBE[3] = byte(block)
		prf.Write(blockBE[:])
		dk = prf.Sum(dk)
		T := dk[len(dk)-hashLen:]
		copy(U, T)
		for n := 2; n <= iter; n++ {
			prf.Reset()
			prf.Write(U)
			U = U[:0]
			U = prf.Sum(U)
			for x := range U {
				T[x] ^= U[x]
			}
		}
	}
	return dk[:keyLen]
}

func hashPassword(password, saltHex string) string {
	salt, _ := hex.DecodeString(saltHex)
	return hex.EncodeToString(pbkDF2([]byte(password), salt, pbkdf2Iters, pbkdf2KeyLen))
}

// ---- 登录限流（内存）----

type loginLimiter struct {
	mu     sync.Mutex
	states map[string]*loginState
}

type loginState struct {
	fails     int
	lockUntil time.Time
	lastFail  time.Time
}

func newLoginLimiter() *loginLimiter {
	return &loginLimiter{states: map[string]*loginState{}}
}

func (l *loginLimiter) locked(name string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	st, ok := l.states[name]
	if !ok {
		return false
	}
	// 距上次失败超过锁窗口 10 倍 → 计数自然过期清零
	if time.Since(st.lastFail) > 2*time.Minute {
		delete(l.states, name)
		return false
	}
	return time.Now().Before(st.lockUntil)
}

func (l *loginLimiter) fail(name string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	for key, v := range l.states {
		if time.Since(v.lastFail) > 2*time.Minute {
			delete(l.states, key)
		}
	}
	if len(l.states) >= 4096 {
		return
	}
	st := l.states[name]
	if st == nil {
		st = &loginState{}
		l.states[name] = st
	}
	st.fails++
	st.lastFail = time.Now()
	if st.fails >= 5 {
		st.lockUntil = time.Now().Add(10 * time.Second)
	}
}

func (l *loginLimiter) success(name string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	delete(l.states, name)
}

// ---- handlers ----

func (a *App) handleRegister(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := readJSON(w, r, &req, 1<<20); err != nil {
		return
	}
	if !usernameRe.MatchString(req.Username) {
		writeJSON(w, http.StatusBadRequest, map[string]string{
			"error": "用户名需 3-32 位字母/数字/下划线/连字符"})
		return
	}
	if len(req.Password) < 6 || len(req.Password) > 1024 {
		writeJSON(w, http.StatusBadRequest, map[string]string{
			"error": "密码需 6-1024 字节"})
		return
	}
	salt, err := randHex(saltBytes)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "服务器内部错误"})
		return
	}
	u, err := a.store.CreateUser(req.Username, salt, hashPassword(req.Password, salt))
	if err != nil {
		if errors.Is(err, os.ErrExist) {
			writeJSON(w, 409, map[string]string{"error": "用户名已被占用"})
		} else {
			writeJSON(w, 500, map[string]string{"error": "注册失败"})
		}
		return
	}
	tok, err := a.store.IssueToken(u.ID)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "服务器内部错误"})
		return
	}
	writeJSON(w, http.StatusCreated, map[string]string{
		"token":    tok,
		"username": u.Name,
	})
}

func (a *App) handleLogin(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := readJSON(w, r, &req, 1<<20); err != nil {
		return
	}
	if !usernameRe.MatchString(req.Username) || len(req.Password) > 1024 {
		writeJSON(w, 400, map[string]string{"error": "用户名或密码格式错误"})
		return
	}
	if a.limiter.locked(req.Username) {
		writeJSON(w, http.StatusTooManyRequests, map[string]string{
			"error": "尝试过于频繁，请 10 秒后再试"})
		return
	}
	u, ok := a.store.FindUser(req.Username)
	salt := u.Salt
	if !ok {
		salt = "00000000000000000000000000000000"
	}
	candidate := hashPassword(req.Password, salt)
	if !ok || subtle.ConstantTimeCompare([]byte(candidate), []byte(u.Hash)) != 1 {
		a.limiter.fail(req.Username)
		writeJSON(w, http.StatusUnauthorized, map[string]string{
			"error": "用户名或密码错误"})
		return
	}
	a.limiter.success(req.Username)
	tok, err := a.store.IssueToken(u.ID)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "服务器内部错误"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{
		"token":    tok,
		"username": u.Name,
	})
}

func (a *App) handleLogout(w http.ResponseWriter, r *http.Request) {
	if tok, ok := bearerToken(r); ok {
		if err := a.store.RevokeToken(tok); err != nil {
			writeJSON(w, 500, map[string]string{"error": "注销失败，请重试"})
			return
		}
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (a *App) handleProfile(w http.ResponseWriter, r *http.Request) {
	uid := r.Context().Value(uidKey{}).(string)
	u, ok := a.store.FindUserByID(uid)
	if !ok {
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "账户不存在"})
		return
	}
	used, err := a.store.Usage(uid)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "统计失败"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"username":   u.Name,
		"usedBytes":  used,
		"quotaBytes": u.QuotaBytes,
	})
}

// requireAuth 中间件：Bearer 校验通过后把 uid 塞进 request context。
type uidKey struct{}

func (a *App) requireAuth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		tok, ok := bearerToken(r)
		if !ok {
			writeJSON(w, http.StatusUnauthorized, map[string]string{
				"error": "未登录或登录已过期"})
			return
		}
		uid, ok := a.store.CheckToken(tok)
		if !ok {
			writeJSON(w, http.StatusUnauthorized, map[string]string{
				"error": "未登录或登录已过期"})
			return
		}
		next(w, r.WithContext(contextWithUID(r, uid)))
	}
}

func contextWithUID(r *http.Request, uid string) context.Context {
	return context.WithValue(r.Context(), uidKey{}, uid)
}

func bearerToken(r *http.Request) (string, bool) {
	h := r.Header.Get("Authorization")
	const prefix = "Bearer "
	if len(h) > len(prefix) && h[:len(prefix)] == prefix {
		return h[len(prefix):], true
	}
	return "", false
}

// readJSON 统一解析请求体（限制大小；错误时已写好响应）。
func readJSON(w http.ResponseWriter, r *http.Request, v interface{}, maxBytes int64) error {
	body := http.MaxBytesReader(w, r.Body, maxBytes)
	dec := json.NewDecoder(body)
	err := dec.Decode(v)
	if err == nil {
		var extra interface{}
		if dec.Decode(&extra) != io.EOF {
			err = errors.New("multiple JSON values")
		}
	}
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{
			"error": "请求格式错误"})
		return err
	}
	return nil
}

func writeJSON(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

// Bound password hashing work per source IP; proxy is configured without trusting arbitrary forwarded headers.
type authRate struct {
	start time.Time
	count int
}

func (a *App) allowAuth(remote string) bool {
	host, _, err := net.SplitHostPort(remote)
	if err != nil {
		host = remote
	}
	a.authMu.Lock()
	defer a.authMu.Unlock()
	if a.authRates == nil {
		a.authRates = map[string]authRate{}
	}
	now := time.Now()
	for key, v := range a.authRates {
		if now.Sub(v.start) > time.Minute {
			delete(a.authRates, key)
		}
	}
	v := a.authRates[host]
	if v.start.IsZero() {
		if len(a.authRates) >= 4096 {
			return false
		}
		v.start = now
	}
	v.count++
	a.authRates[host] = v
	return v.count <= 120
}
