package main

// dutyon-cloud — DutyOn 云端账户服务（纯标准库，单二进制）。
//
// 用法：
//   dutyon-cloud [--addr :8787] [--data ./data]     启动服务
//   dutyon-cloud --publish <DutyOn-update-vX.Y.Z.zip> [--notes "..."] [--data ./data]
//                                                    发布升级包后退出
//
// 路由（详见各 handler 注释；鉴权 = Authorization: Bearer <token>）：
//   POST /api/auth/register|login|logout   GET /api/profile
//   GET|PUT /api/sync/config               GET /api/sync/manifest
//   GET|PUT|DELETE /api/sync/file?path=...
//   GET /api/app/version|download          （无鉴权）

import (
	"context"
	"flag"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"
)

type App struct {
	store        *Store
	limiter      *loginLimiter
	authMu       sync.Mutex
	authRates    map[string]authRate
	trustedProxy *net.IPNet
	authSlots    chan struct{}
	fileSlots    chan struct{}
}

func main() {
	addr := flag.String("addr", ":8787", "HTTP 监听地址")
	data := flag.String("data", "./data", "数据目录（JSON 存储）")
	healthcheck := flag.String("healthcheck", "", "探测指定健康检查 URL 后退出")
	publish := flag.String("publish", "", "发布升级包（zip 路径）后退出")
	kind := flag.String("kind", "update", "发布类型：update / installer / bundle")
	proxy := flag.String("trusted-proxy", "", "可信反向代理 CIDR（仅该网段可传 X-Real-IP）")
	notes := flag.String("notes", "", "升级说明（配合 --publish）")
	flag.Parse()
	if *healthcheck != "" {
		client := &http.Client{Timeout: 3 * time.Second}
		resp, err := client.Get(*healthcheck)
		if err != nil {
			log.Fatal(err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			log.Fatalf("healthcheck: HTTP %d", resp.StatusCode)
		}
		return
	}

	store, err := OpenStore(*data)
	if err != nil {
		log.Fatalf("打开数据目录失败: %v", err)
	}
	app := &App{store: store, limiter: newLoginLimiter()}
	if *proxy != "" {
		_, subnet, err := net.ParseCIDR(*proxy)
		if err != nil {
			log.Fatal(err)
		}
		app.trustedProxy = subnet
	}

	if *publish != "" {
		if err := app.publishArtifact(*publish, *notes, *kind); err != nil {
			log.Fatalf("发布失败: %v", err)
		}
		rel, _ := app.loadReleaseKind(*kind)
		log.Printf("已发布 v%s（%.1f MB）\n  sha256: %s\n",
			rel.Version, float64(rel.Size)/1048576.0, rel.SHA256)
		return
	}

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	srv := &http.Server{Addr: *addr, Handler: app.Handler(), ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout: 2 * time.Minute, WriteTimeout: 3 * time.Minute, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 32 << 10}
	go func() {
		<-ctx.Done()
		shutdown, done := context.WithTimeout(context.Background(), 10*time.Second)
		defer done()
		_ = srv.Shutdown(shutdown)
	}()
	log.Printf("dutyon-cloud listening on %s", *addr)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}

func (app *App) Handler() http.Handler {
	app.authSlots = make(chan struct{}, 4)
	app.fileSlots = make(chan struct{}, 4)
	mux := http.NewServeMux()
	// 账户
	mux.HandleFunc("/api/auth/register", method("POST", app.handleRegister))
	mux.HandleFunc("/api/auth/login", method("POST", app.handleLogin))
	mux.HandleFunc("/api/auth/logout", method("POST", app.handleLogout))
	mux.HandleFunc("/api/profile", app.requireAuth(method("GET", app.handleProfile)))
	// 同步
	mux.HandleFunc("/api/sync/config", app.requireAuth(
		func(w http.ResponseWriter, r *http.Request) {
			switch r.Method {
			case http.MethodGet:
				app.handleConfigGet(w, r)
			case http.MethodPut:
				app.handleConfigPut(w, r)
			default:
				w.Header().Set("Allow", "GET, PUT")
				writeJSON(w, http.StatusMethodNotAllowed,
					map[string]string{"error": "method not allowed"})
			}
		}))
	mux.HandleFunc("/api/sync/manifest", app.requireAuth(
		func(w http.ResponseWriter, r *http.Request) {
			if r.Method != http.MethodGet {
				w.Header().Set("Allow", "GET")
				writeJSON(w, http.StatusMethodNotAllowed,
					map[string]string{"error": "method not allowed"})
				return
			}
			app.handleManifest(w, r)
		}))
	mux.HandleFunc("/api/sync/file", app.requireAuth(
		func(w http.ResponseWriter, r *http.Request) {
			switch r.Method {
			case http.MethodGet:
				app.handleFileGet(w, r)
			case http.MethodPut:
				app.handleFilePut(w, r)
			case http.MethodDelete:
				app.handleFileDelete(w, r)
			default:
				w.Header().Set("Allow", "GET, PUT, DELETE")
				writeJSON(w, http.StatusMethodNotAllowed,
					map[string]string{"error": "method not allowed"})
			}
		}))
	// 升级（无鉴权）
	mux.HandleFunc("/api/app/version", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", "GET")
			writeJSON(w, http.StatusMethodNotAllowed,
				map[string]string{"error": "method not allowed"})
			return
		}
		app.handleVersion(w, r)
	})
	mux.HandleFunc("/api/app/download", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", "GET")
			writeJSON(w, http.StatusMethodNotAllowed,
				map[string]string{"error": "method not allowed"})
			return
		}
		app.handleDownload(w, r)
	})

	mux.HandleFunc("/healthz", method("GET", func(w http.ResponseWriter, r *http.Request) { writeJSON(w, 200, map[string]bool{"ok": true}) }))
	mux.HandleFunc("/api/app/releases", method("GET", app.handleReleases))
	mux.HandleFunc("/api/app/artifact", method("GET", app.handleArtifact))
	mux.HandleFunc("/", app.handleWeb)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Cache-Control", "no-store")
		if strings.HasPrefix(r.URL.Path, "/api/auth/") {
			remote := r.RemoteAddr
			host, _, _ := net.SplitHostPort(remote)
			if app.trustedProxy != nil && app.trustedProxy.Contains(net.ParseIP(host)) {
				if ip := net.ParseIP(r.Header.Get("X-Real-IP")); ip != nil {
					remote = ip.String()
				}
			}
			if !app.allowAuth(remote) {
				writeJSON(w, 429, map[string]string{"error": "请求过于频繁，请稍后重试"})
				return
			}
			select {
			case app.authSlots <- struct{}{}:
				defer func() { <-app.authSlots }()
			default:
				writeJSON(w, 503, map[string]string{"error": "服务繁忙，请稍后重试"})
				return
			}
		}
		if r.URL.Path == "/api/sync/file" {
			select {
			case app.fileSlots <- struct{}{}:
				defer func() { <-app.fileSlots }()
			default:
				writeJSON(w, 503, map[string]string{"error": "服务繁忙，请稍后重试"})
				return
			}
		}
		mux.ServeHTTP(w, r)
	})
}

func method(allowed string, next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != allowed {
			w.Header().Set("Allow", allowed)
			writeJSON(w, 405, map[string]string{"error": "method not allowed"})
			return
		}
		next(w, r)
	}
}
