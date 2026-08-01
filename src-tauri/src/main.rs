// Prevents additional console window on Windows in release. In dev the console
// stays open for log output.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // Fix transparent window rendering on RDP / systems without GPU compositing.
    // WebView2's transparent background relies on GPU compositing, which is often
    // unavailable on Remote Desktop. --disable-gpu forces software compositing,
    // which handles layered transparent windows correctly.
    // WebView2 is Windows-only; on macOS (WKWebView) / Linux (WebKitGTK) this env
    // var is meaningless, so gate it to Windows.
    #[cfg(windows)]
    if std::env::var("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS").is_err() {
        std::env::set_var(
            "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
            "--disable-gpu",
        );
    }
    duty_on_lib::run();
}
