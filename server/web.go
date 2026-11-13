package main

import (
	"embed"
	"io/fs"
	"net/http"
	"path"
	"strings"
)

// 网页资源随二进制一起分发：web/ 下的页面、脚本、样式与 web/img/ 下的图片。
//
//go:embed web
var webAssets embed.FS

// 页面只加载本站资源。blob: 仅用于云空间里预览用户自己的图片和音频
// （这些文件要带登录令牌取回，不能直接写成地址）。
const webCSP = "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; " +
	"img-src 'self' blob:; media-src blob:; base-uri 'none'; form-action 'self'; frame-ancestors 'none'"

func (a *App) handleWeb(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	assets, _ := fs.Sub(webAssets, "web")
	name := strings.TrimPrefix(path.Clean(r.URL.Path), "/")
	if name == "" || name == "." {
		name = "index.html"
	}
	// 只发确实存在的文件，不列目录
	if info, err := fs.Stat(assets, name); err != nil || info.IsDir() {
		http.NotFound(w, r)
		return
	}
	h := w.Header()
	h.Set("Content-Security-Policy", webCSP)
	h.Set("X-Content-Type-Options", "nosniff")
	h.Set("Referrer-Policy", "no-referrer")
	if strings.HasPrefix(name, "img/") {
		h.Set("Cache-Control", "public, max-age=86400")
	} else {
		// 页面、脚本、样式每次向服务器确认，升级后不会新旧混用
		h.Set("Cache-Control", "no-cache")
	}
	http.ServeFileFS(w, r, assets, name)
}
