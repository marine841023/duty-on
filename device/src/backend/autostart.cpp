// 开机自启动注册表实现（仅 Windows）。

#ifdef _WIN32

#include "backend/autostart.h"

#include <windows.h>

namespace dutyon::backend {

namespace {
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kAutostartName = L"DutyOn";
} // namespace

bool autostartEnabled() {
    DWORD type = 0;
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    const LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAutostartName, RRF_RT_REG_SZ,
                                    &type, buf, &size);
    return rc == ERROR_SUCCESS;
}

bool setAutostartEnabled(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LSTATUS rc;
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        rc = RegSetValueExW(key, kAutostartName, 0, REG_SZ, (const BYTE*)exe,
                            (DWORD)((wcslen(exe) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kAutostartName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;  // 幂等删除
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

} // namespace dutyon::backend

#endif // _WIN32
