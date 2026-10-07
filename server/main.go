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
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
)

type App struct {
	store   *Store
	limiter *loginLimiter
}

func main() {
	addr := flag.String("addr", ":8787", "HTTP 监听地址")
	data := flag.String("data", "./data", "数据目录（JSON 存储）")
	publish := flag.String("publish", "", "发布升级包（zip 路径）后退出")
	notes := flag.String("notes", "", "升级说明（配合 --publish）")
	flag.Parse()

	store, err := OpenStore(*data)
	if err != nil {
		log.Fatalf("打开数据目录失败: %v", err)
	}
	app := &App{store: store, limiter: newLoginLimiter()}

	if *publish != "" {
		if err := app.publish(*publish, *notes); err != nil {
			log.Fatalf("发布失败: %v", err)
		}
		rel, _ := app.loadRelease()
		fmt.Printf("已发布 v%s（%.1f MB）\n  sha256: %s\n",
			rel.Version, float64(rel.Size)/1048576.0, rel.SHA256)
		return
	}

	mux := http.NewServeMux()
	// 账户
	mux.HandleFunc("/api/auth/register", app.handleRegister)
	mux.HandleFunc("/api/auth/login", app.handleLogin)
	mux.HandleFunc("/api/auth/logout", app.handleLogout)
	mux.HandleFunc("/api/profile", app.requireAuth(app.handleProfile))
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

	log.Printf("dutyon-cloud 监听 %s（数据目录 %s）", *addr, *data)
	if err := http.ListenAndServe(*addr, mux); err != nil {
		fmt.Fprintln(os.Stderr, "服务退出:", err)
		os.Exit(1)
	}
}
