#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <usbiodef.h>
#include <winusb.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "ole32.lib")

namespace {
constexpr USHORT kVid = 0x1CBE;
constexpr USHORT kPid = 0x0050;

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

bool contains(const std::wstring& value, const wchar_t* needle) {
    std::wstring upper = value;
    std::transform(upper.begin(), upper.end(), upper.begin(), towupper);
    std::wstring target = needle;
    std::transform(target.begin(), target.end(), target.begin(), towupper);
    return upper.find(target) != std::wstring::npos;
}

void inspectWinUsbPath(const std::wstring& path, bool& found) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("WinUSB interface open failed: %lu\n", GetLastError());
        return;
    }
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(handle, &usb)) {
        std::printf("USB device interface: %s (WinUSB unavailable: %lu)\n",
                    narrow(path).c_str(), GetLastError());
        CloseHandle(handle);
        return;
    }
    found = true;
    USB_INTERFACE_DESCRIPTOR descriptor{};
    if (WinUsb_QueryInterfaceSettings(usb, 0, &descriptor)) {
        std::printf("USB device interface: %s, class=0x%02X endpoints=%u\n",
                    narrow(path).c_str(), descriptor.bInterfaceClass,
                    descriptor.bNumEndpoints);
        for (UCHAR ep = 0; ep < descriptor.bNumEndpoints; ++ep) {
            WINUSB_PIPE_INFORMATION pipe{};
            if (WinUsb_QueryPipe(usb, 0, ep, &pipe))
                std::printf("  endpoint=0x%02X type=%u maxPacket=%u\n",
                            pipe.PipeId, pipe.PipeType, pipe.MaximumPacketSize);
        }
    }
    WinUsb_Free(usb);
    CloseHandle(handle);
}

bool resetWinUsbPath(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("WinUSB reset open failed: %lu\n", GetLastError());
        return false;
    }
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(handle, &usb)) {
        std::printf("WinUSB reset initialize failed: %lu\n", GetLastError());
        CloseHandle(handle);
        return false;
    }
    USB_INTERFACE_DESCRIPTOR descriptor{};
    bool success = WinUsb_QueryInterfaceSettings(usb, 0, &descriptor) != FALSE;
    for (UCHAR index = 0; success && index < descriptor.bNumEndpoints; ++index) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(usb, 0, index, &pipe)) continue;
        if (pipe.PipeType != UsbdPipeTypeBulk) continue;
        WinUsb_AbortPipe(usb, pipe.PipeId);
        const BOOL reset = WinUsb_ResetPipe(usb, pipe.PipeId);
        std::printf("WinUSB reset endpoint 0x%02X: %s (%lu)\n",
                    pipe.PipeId, reset ? "ok" : "failed", reset ? 0UL : GetLastError());
        success = reset != FALSE && success;
    }
    WinUsb_Free(usb);
    CloseHandle(handle);
    return success;
}

void printUsbInterfaces() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr,
                                         DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &data); ++index) {
        wchar_t hardware[4096]{};
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID,
                                               &type, reinterpret_cast<PBYTE>(hardware),
                                               sizeof(hardware), nullptr))
            continue;
        std::wstring ids(hardware);
        if (!contains(ids, L"VID_1CBE&PID_0050")) continue;
        wchar_t name[512]{};
        SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_FRIENDLYNAME, &type,
                                          reinterpret_cast<PBYTE>(name), sizeof(name), nullptr);
        std::printf("USB interface: %s\n", narrow(ids).c_str());
        if (name[0]) std::printf("  name: %s\n", narrow(name).c_str());
        wchar_t instanceId[4096]{};
        if (SetupDiGetDeviceInstanceIdW(set, &data, instanceId,
                                        static_cast<DWORD>(std::size(instanceId)), nullptr)) {
            std::printf("  instance: %s\n", narrow(instanceId).c_str());
        }
        DWORD status = 0;
        DWORD problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) == CR_SUCCESS) {
            std::printf("  status: 0x%08lX problem: %lu\n",
                        static_cast<unsigned long>(status),
                        static_cast<unsigned long>(problem));
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}

bool probeWinUsb() {
    std::printf("Checking registered WinUSB interfaces.\n");
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr,
                                        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &data); ++index) {
        wchar_t hardware[4096]{};
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID,
                                               &type, reinterpret_cast<PBYTE>(hardware),
                                               sizeof(hardware), nullptr))
            continue;
        if (!contains(hardware, L"VID_1CBE&PID_0050")) continue;

        HKEY key = SetupDiOpenDevRegKey(set, &data, DICS_FLAG_GLOBAL, 0,
                                        DIREG_DEV, KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE) continue;
        HKEY parameters = nullptr;
        const LONG opened = RegOpenKeyExW(key, L"Device Parameters", 0,
                                          KEY_QUERY_VALUE, &parameters);
        wchar_t guidText[64]{};
        DWORD guidBytes = sizeof(guidText);
        DWORD guidType = 0;
        wchar_t symbolicText[512]{};
        DWORD symbolicBytes = sizeof(symbolicText);
        DWORD symbolicType = 0;
        const LONG queried = opened == ERROR_SUCCESS
            ? RegQueryValueExW(parameters, L"DeviceInterfaceGUID", nullptr, &guidType,
                                reinterpret_cast<LPBYTE>(guidText), &guidBytes)
            : ERROR_FILE_NOT_FOUND;
        const LONG symbolicQueried = opened == ERROR_SUCCESS
            ? RegQueryValueExW(parameters, L"SymbolicName", nullptr, &symbolicType,
                                reinterpret_cast<LPBYTE>(symbolicText), &symbolicBytes)
            : ERROR_FILE_NOT_FOUND;
        if (opened != ERROR_SUCCESS || queried != ERROR_SUCCESS)
            std::printf("WinUSB registry lookup failed: open=%ld guid=%ld symbolic=%ld\n",
                        opened, queried, symbolicQueried);
        if (parameters) RegCloseKey(parameters);
        RegCloseKey(key);
        if (queried != ERROR_SUCCESS || guidType != REG_SZ) {
            // The generic WinUSB binding used by this device does not expose
            // Device Parameters through SetupDiOpenDevRegKey on all Windows
            // builds. Its interface GUID is stable and comes from winusb.inf.
            wcscpy_s(guidText, L"{88BAE032-5A81-49f0-BC3D-A4FF138216D6}");
            guidType = REG_SZ;
        }
        std::printf("WinUSB interface GUID: %s\n", narrow(guidText).c_str());
        if (symbolicQueried == ERROR_SUCCESS && symbolicType == REG_SZ)
            std::printf("WinUSB symbolic link: %s\n", narrow(symbolicText).c_str());

        GUID interfaceGuid{};
        if (CLSIDFromString(guidText, &interfaceGuid) != S_OK) continue;
        HDEVINFO interfaces = SetupDiGetClassDevsW(&interfaceGuid, nullptr, nullptr,
                                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (interfaces == INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        for (DWORD interfaceIndex = 0;
             SetupDiEnumDeviceInterfaces(interfaces, nullptr, &interfaceGuid,
                                         interfaceIndex, &interfaceData);
             ++interfaceIndex) {
            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(interfaces, &interfaceData, nullptr, 0,
                                             &required, nullptr);
            if (!required) continue;
            std::vector<unsigned char> buffer(required);
            auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(interfaces, &interfaceData, detail,
                                                  required, nullptr, nullptr)) continue;
            std::wstring path(detail->DevicePath);
            if (!contains(path, L"VID_1CBE&PID_0050")) continue;
            inspectWinUsbPath(path, found);
        }
        SetupDiDestroyDeviceInfoList(interfaces);

        // WinUSB also stores the generic USB symbolic link even when a custom
        // DeviceInterfaceGUID has no enumerated interface in SetupAPI.
        if (!found && symbolicQueried == ERROR_SUCCESS && symbolicType == REG_SZ) {
            std::wstring path(symbolicText);
            if (path.rfind(L"\\??\\", 0) == 0)
                path.replace(0, 4, L"\\\\?\\");
            if (contains(path, L"VID_1CBE&PID_0050"))
                inspectWinUsbPath(path, found);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!found) std::printf("No registered TURZX WinUSB interface found.\n");
    return found;
}

bool transferReport(HANDLE handle, std::vector<unsigned char>& report,
                    bool writing, DWORD& transferred) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    const BOOL started = writing
        ? WriteFile(handle, report.data(), static_cast<DWORD>(report.size()),
                    &transferred, &overlapped)
        : ReadFile(handle, report.data(), static_cast<DWORD>(report.size()),
                   &transferred, &overlapped);
    bool complete = started != FALSE;
    DWORD error = complete ? ERROR_SUCCESS : GetLastError();
    if (!complete && error == ERROR_IO_PENDING) {
        const DWORD waited = WaitForSingleObject(event, 1000);
        if (waited == WAIT_OBJECT_0) {
            complete = GetOverlappedResult(handle, &overlapped, &transferred, FALSE) != FALSE;
            error = complete ? ERROR_SUCCESS : GetLastError();
        } else {
            error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CancelIoEx(handle, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(handle, &overlapped, &ignored, TRUE);
        }
    }
    CloseHandle(event);
    SetLastError(error);
    return complete;
}

bool probeHid(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("HID open failed: %lu\n", GetLastError());
        return false;
    }
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    HIDP_CAPS caps{};
    if (HidD_GetPreparsedData(handle, &preparsed) &&
        HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
        std::printf("HID report lengths: input=%u output=%u feature=%u\n",
                    caps.InputReportByteLength, caps.OutputReportByteLength,
                    caps.FeatureReportByteLength);
    }
    if (preparsed) HidD_FreePreparsedData(preparsed);
    const ULONG reportLength = caps.OutputReportByteLength;
    if (reportLength < 4 || reportLength > 4096) {
        CloseHandle(handle);
        return false;
    }
    std::vector<unsigned char> report(reportLength, 0);
    report[1] = 0xAA;
    report[2] = 0x55;
    report[3] = 0x33;
    DWORD written = 0;
    if (!transferReport(handle, report, true, written) || written != report.size()) {
        std::printf("identity query failed: error=%lu bytes=%lu\n", GetLastError(), written);
        CloseHandle(handle);
        return false;
    }
    std::printf("identity query sent: AA 55 33\n");

    if (!caps.InputReportByteLength || caps.InputReportByteLength > 4096) {
        CloseHandle(handle);
        return false;
    }
    std::vector<unsigned char> input(caps.InputReportByteLength, 0);
    DWORD received = 0;
    const bool complete = transferReport(handle, input, false, received);
    if (complete) {
        std::printf("identity response: %lu bytes\n", received);
        const DWORD displayed = std::min<DWORD>(received, 32);
        for (DWORD index = 0; index < displayed; ++index)
            std::printf("%02X%s", input[index], (index + 1) % 16 ? "-" : "\n");
        if (displayed % 16) std::printf("\n");
    } else {
        std::printf("identity response unavailable: %lu\n", GetLastError());
    }
    CloseHandle(handle);
    return complete && received > 1 && input[0] == 0 && input[1] == 0xAA;
}
}

int wmain(int argc, wchar_t** argv) {
    std::printf("DutyOn TURZX probe VID_%04X&PID_%04X\n", kVid, kPid);
    printUsbInterfaces();
    const bool winUsbFound = probeWinUsb();
    if (argc > 1 && _wcsicmp(argv[1], L"--reset") == 0) {
        GUID interfaceGuid{};
        CLSIDFromString(L"{88BAE032-5A81-49F0-BC3D-A4FF138216D6}", &interfaceGuid);
        HDEVINFO interfaces = SetupDiGetClassDevsW(&interfaceGuid, nullptr, nullptr,
                                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        bool reset = false;
        if (interfaces != INVALID_HANDLE_VALUE) {
            SP_DEVICE_INTERFACE_DATA data{};
            data.cbSize = sizeof(data);
            for (DWORD index = 0; SetupDiEnumDeviceInterfaces(interfaces, nullptr, &interfaceGuid,
                                                               index, &data); ++index) {
                DWORD required = 0;
                SetupDiGetDeviceInterfaceDetailW(interfaces, &data, nullptr, 0, &required, nullptr);
                if (!required) continue;
                std::vector<unsigned char> buffer(required);
                auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(interfaces, &data, detail, required,
                                                      nullptr, nullptr)) continue;
                if (contains(detail->DevicePath, L"VID_1CBE&PID_0050")) {
                    reset = resetWinUsbPath(detail->DevicePath) || reset;
                }
            }
            SetupDiDestroyDeviceInfoList(interfaces);
        }
        return reset ? 0 : 4;
    }
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 1;
    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, index,
                                                      &interfaceData); ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, nullptr, 0, &required, nullptr);
        if (!required) continue;
        std::vector<unsigned char> buffer(required);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, detail, required,
                                              nullptr, nullptr)) continue;
        std::wstring path(detail->DevicePath);
        if (!contains(path, L"VID_1CBE&PID_0050")) continue;
        HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(handle, &attributes) &&
            attributes.VendorID == kVid && attributes.ProductID == kPid) {
            std::printf("HID interface: %s\n", narrow(path).c_str());
            CloseHandle(handle);
            const bool success = probeHid(path);
            SetupDiDestroyDeviceInfoList(set);
            return success || winUsbFound ? 0 : 3;
        }
        CloseHandle(handle);
    }
    SetupDiDestroyDeviceInfoList(set);
    std::printf("No present TURZX HID interface found.\n");
    return winUsbFound ? 0 : 2;
}

#else
int main() { return 0; }
#endif
