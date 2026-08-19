// Prevents additional console window on Windows in release. In dev the console
// stays open for log output.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    duty_on_lib::run();
}
