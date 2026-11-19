#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include "platform/turzx_display_win32.h"

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usbiodef.h>
#include <objbase.h>
#include <bcrypt.h>

#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

namespace dutyon {
namespace {

constexpr USHORT kVid = 0x1CBE;
constexpr USHORT kPid = 0x0050;
constexpr UCHAR kBulkOut = 0x01;
constexpr UCHAR kBulkIn = 0x81;
constexpr size_t kBlockSize = 512;
constexpr size_t kTransferChunk = 16 * 1024;
constexpr int kOutputWidth = 720;
constexpr int kOutputHeight = 1280;
constexpr auto kMinFrameInterval = std::chrono::milliseconds(100);
constexpr auto kReconnectDelay = std::chrono::milliseconds(1000);

bool hasDeviceId(const std::wstring& value) {
    std::wstring upper = value;
    std::transform(upper.begin(), upper.end(), upper.begin(), towupper);
    return upper.find(L"VID_1CBE&PID_0050") != std::wstring::npos;
}

std::wstring findDevicePath() {
    GUID guid{};
    if (CLSIDFromString(L"{88BAE032-5A81-49f0-BC3D-A4FF138216D6}", &guid) != S_OK)
        return {};
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::wstring path;
    SP_DEVICE_INTERFACE_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &data);
         ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &required, nullptr);
        if (!required) continue;
        std::vector<std::uint8_t> buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, required, nullptr, nullptr) &&
            hasDeviceId(detail->DevicePath)) {
            path = detail->DevicePath;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return path;
}

bool encryptHeader(const std::uint8_t command, std::uint32_t pngLength,
                   std::vector<std::uint8_t>& header) {
    header.assign(kBlockSize, 0);
    header[0] = command;
    header[2] = 26;
    header[3] = 109;
    const auto timestamp = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::memcpy(header.data() + 4, &timestamp, sizeof(timestamp));
    header[8] = static_cast<std::uint8_t>(pngLength >> 24);
    header[9] = static_cast<std::uint8_t>(pngLength >> 16);
    header[10] = static_cast<std::uint8_t>(pngLength >> 8);
    header[11] = static_cast<std::uint8_t>(pngLength);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_DES_ALGORITHM,
                                                   nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        std::fprintf(stderr, "[TURZX] BCryptOpenAlgorithmProvider failed: 0x%08lX\n",
                     static_cast<unsigned long>(status));
        return false;
    }
    const wchar_t mode[] = BCRYPT_CHAIN_MODE_CBC;
    status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                               reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
                               sizeof(mode), 0);
    DWORD objectLength = 0;
    DWORD resultLength = 0;
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&objectLength),
                                   sizeof(objectLength), &resultLength, 0);
    }
    std::vector<UCHAR> keyObject(objectLength);
    const UCHAR desKey[8] = {'s', 'l', 'v', '3', 't', 'u', 'z', 'x'};
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptGenerateSymmetricKey(algorithm, &key, keyObject.data(),
                                             objectLength, const_cast<PUCHAR>(desKey),
                                             sizeof(desKey), 0);
    }
    std::vector<UCHAR> plaintext(header.begin(), header.begin() + 500);
    std::vector<UCHAR> encrypted(512, 0);
    ULONG encryptedLength = 0;
    UCHAR iv[8] = {'s', 'l', 'v', '3', 't', 'u', 'z', 'x'};
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptEncrypt(key, plaintext.data(), static_cast<ULONG>(plaintext.size()),
                               nullptr, iv, sizeof(iv), encrypted.data(),
                               static_cast<ULONG>(encrypted.size()), &encryptedLength,
                               BCRYPT_BLOCK_PADDING);
    }
    if (!BCRYPT_SUCCESS(status) || encryptedLength != 504) {
        std::fprintf(stderr, "[TURZX] BCryptEncrypt failed: 0x%08lX length=%lu\n",
                     static_cast<unsigned long>(status), encryptedLength);
        if (key) BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::memcpy(header.data(), encrypted.data(), encryptedLength);
    header[510] = 0xA1;
    header[511] = 0x1A;
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return true;
}

void pngWriteCallback(void* context, void* data, int size) {
    auto* output = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    output->insert(output->end(), bytes, bytes + size);
}

} // namespace

struct TurzxDisplay::Impl {
    HANDLE device = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    UCHAR outPipe = kBulkOut;
    UCHAR inPipe = kBulkIn;
    std::atomic<bool> running{false};
    std::atomic<bool> online{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::uint8_t> pending;
    int pendingWidth = 0;
    int pendingHeight = 0;
    std::chrono::steady_clock::time_point lastTransfer{};
    std::thread worker;

    void resetPipes() {
        if (!usb) return;
        WinUsb_AbortPipe(usb, outPipe);
        WinUsb_ResetPipe(usb, outPipe);
        WinUsb_AbortPipe(usb, inPipe);
        WinUsb_ResetPipe(usb, inPipe);
    }

    bool open() {
        const std::wstring path = findDevicePath();
        if (path.empty()) {
            std::fprintf(stderr, "[TURZX] device interface not found\n");
            return false;
        }
        device = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (device == INVALID_HANDLE_VALUE || !WinUsb_Initialize(device, &usb)) {
            std::fprintf(stderr, "[TURZX] open failed: %lu\n", GetLastError());
            if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
            device = INVALID_HANDLE_VALUE;
            usb = nullptr;
            return false;
        }
        ULONG timeout = 2000;
        WinUsb_SetPipePolicy(usb, outPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        WinUsb_SetPipePolicy(usb, inPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        USB_INTERFACE_DESCRIPTOR descriptor{};
        if (WinUsb_QueryInterfaceSettings(usb, 0, &descriptor)) {
            for (UCHAR i = 0; i < descriptor.bNumEndpoints; ++i) {
                WINUSB_PIPE_INFORMATION pipe{};
                if (!WinUsb_QueryPipe(usb, 0, i, &pipe)) continue;
                if (pipe.PipeType != UsbdPipeTypeBulk) continue;
                if ((pipe.PipeId & 0x80) != 0) inPipe = pipe.PipeId;
                else outPipe = pipe.PipeId;
            }
        }
        online = true;
        std::fprintf(stderr, "[TURZX] connected (out=0x%02X in=0x%02X)\n", outPipe, inPipe);
        return true;
    }

    void close() {
        online = false;
        if (usb) WinUsb_Free(usb);
        usb = nullptr;
        if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
        device = INVALID_HANDLE_VALUE;
    }

    void cancelTransfer(OVERLAPPED& overlapped, UCHAR pipe) {
        if (usb) WinUsb_AbortPipe(usb, pipe);
        if (device != INVALID_HANDLE_VALUE) CancelIoEx(device, &overlapped);
        if (overlapped.hEvent) WaitForSingleObject(overlapped.hEvent, 1000);
    }

    bool writeChunk(const std::uint8_t* data, size_t length) {
        if (!usb || length == 0 || length > static_cast<size_t>(std::numeric_limits<ULONG>::max()))
            return false;
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        ULONG written = 0;
        BOOL started = WinUsb_WritePipe(usb, outPipe, const_cast<PUCHAR>(data),
                                        static_cast<ULONG>(length), &written, &overlapped);
        DWORD error = started ? ERROR_SUCCESS : GetLastError();
        bool complete = started != FALSE;
        if (!complete && error == ERROR_IO_PENDING) {
            const DWORD waited = WaitForSingleObject(overlapped.hEvent, 5000);
            if (waited == WAIT_OBJECT_0) {
                complete = WinUsb_GetOverlappedResult(usb, &overlapped, &written, FALSE) != FALSE;
                error = complete ? ERROR_SUCCESS : GetLastError();
            } else {
                error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                cancelTransfer(overlapped, outPipe);
            }
        }
        CloseHandle(overlapped.hEvent);
        if (!complete || written != length) {
            std::fprintf(stderr, "[TURZX] write failed: error=%lu bytes=%lu/%zu\n",
                         error, written, length);
            SetLastError(error);
            return false;
        }
        return true;
    }

    bool writeAll(const std::uint8_t* data, size_t length) {
        if (!data || length == 0) return false;
        size_t offset = 0;
        while (offset < length) {
            const size_t chunk = std::min(kTransferChunk, length - offset);
            if (!writeChunk(data + offset, chunk)) {
                std::fprintf(stderr, "[TURZX] chunk write failed at %zu/%zu\n",
                             offset, length);
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    bool readFrameAck() {
        if (!usb) return false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(2500);
        while (std::chrono::steady_clock::now() < deadline) {
            std::array<std::uint8_t, kBlockSize> response{};
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent) return false;
            ULONG received = 0;
            BOOL started = WinUsb_ReadPipe(usb, inPipe, response.data(),
                                           static_cast<ULONG>(response.size()),
                                           &received, &overlapped);
            DWORD error = started ? ERROR_SUCCESS : GetLastError();
            bool complete = started != FALSE;
            if (!complete && error == ERROR_IO_PENDING) {
                const DWORD waited = WaitForSingleObject(overlapped.hEvent, 2500);
                if (waited == WAIT_OBJECT_0) {
                    complete = WinUsb_GetOverlappedResult(usb, &overlapped,
                                                          &received, FALSE) != FALSE;
                    error = complete ? ERROR_SUCCESS : GetLastError();
                } else {
                    error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                    cancelTransfer(overlapped, inPipe);
                }
            }
            CloseHandle(overlapped.hEvent);
            if (!complete) {
                std::fprintf(stderr, "[TURZX] frame ack failed: error=%lu bytes=%lu\n",
                             error, received);
                SetLastError(error);
                return false;
            }
            if (received == 0) {
                Sleep(5);
                continue;
            }
            if (response[0] != 0x66 && received >= 2) {
                std::fprintf(stderr, "[TURZX] unexpected frame ack: %02X %02X (%lu bytes)\n",
                             response[0], response[1], received);
            }
            return true;
        }
        std::fprintf(stderr, "[TURZX] frame ack timed out\n");
        WinUsb_AbortPipe(usb, inPipe);
        return false;
    }

    bool sendFrame(const std::vector<std::uint8_t>& rgba) {
        std::vector<std::uint8_t> png;
        png.reserve(rgba.size() / 3);
        stbi_write_png_compression_level = 8;
        if (!stbi_write_png_to_func(pngWriteCallback, &png, kOutputWidth,
                                    kOutputHeight, 4, rgba.data(),
                                    kOutputWidth * 4)) {
            std::fprintf(stderr, "[TURZX] PNG encoding failed\n");
            return false;
        }
        std::vector<std::uint8_t> header;
        if (!encryptHeader(0x66, static_cast<std::uint32_t>(png.size()), header)) {
            std::fprintf(stderr, "[TURZX] header encryption failed\n");
            return false;
        }
        std::vector<std::uint8_t> packet;
        packet.reserve(header.size() + png.size());
        packet.insert(packet.end(), header.begin(), header.end());
        packet.insert(packet.end(), png.begin(), png.end());
        std::fprintf(stderr, "[TURZX] sending PNG frame: %zu bytes (png=%zu)\n",
                     packet.size(), png.size());
        if (!writeAll(packet.data(), packet.size())) return false;
        return readFrameAck();
    }

    void run() {
        while (running) {
            std::vector<std::uint8_t> rgba;
            int width = 0, height = 0;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return !running || !pending.empty(); });
                if (!running) break;
                rgba.swap(pending);
                width = pendingWidth;
                height = pendingHeight;
            }
            if (!online && !open()) {
                std::this_thread::sleep_for(kReconnectDelay);
                continue;
            }
            if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * height * 4)
                continue;
            if (lastTransfer.time_since_epoch().count() != 0) {
                const auto elapsed = std::chrono::steady_clock::now() - lastTransfer;
                if (elapsed < kMinFrameInterval)
                    std::this_thread::sleep_for(kMinFrameInterval - elapsed);
            }
            std::vector<std::uint8_t> frame(static_cast<size_t>(kOutputWidth) *
                                             kOutputHeight * 4u);

            struct Sample {
                int low;
                int high;
                std::uint32_t weight;
            };
            std::vector<Sample> xSamples(kOutputWidth);
            std::vector<Sample> ySamples(kOutputHeight);
            const auto makeSample = [](int destination, int destinationSize,
                                       int sourceSize) {
                const double position =
                    (static_cast<double>(destination) + 0.5) * sourceSize /
                        destinationSize - 0.5;
                const int low = std::clamp(static_cast<int>(position), 0,
                                           sourceSize - 1);
                const int high = std::min(low + 1, sourceSize - 1);
                const double fraction = std::clamp(position - low, 0.0, 1.0);
                return Sample{low, high,
                              static_cast<std::uint32_t>(fraction * 65536.0 + 0.5)};
            };
            for (int x = 0; x < kOutputWidth; ++x)
                xSamples[x] = makeSample(x, kOutputWidth, width);
            for (int y = 0; y < kOutputHeight; ++y) {
                const int sourceRow = kOutputHeight - 1 - y;
                ySamples[y] = makeSample(sourceRow, kOutputHeight, height);
            }
            for (int y = 0; y < kOutputHeight; ++y) {
                const auto& ySample = ySamples[y];
                auto* destination = frame.data() +
                    static_cast<size_t>(y) * kOutputWidth * 4;
                for (int x = 0; x < kOutputWidth; ++x) {
                    const auto& xSample = xSamples[x];
                    const auto* top = rgba.data() +
                        (static_cast<size_t>(ySample.low) * width +
                         xSample.low) * 4;
                    const auto* topRight = rgba.data() +
                        (static_cast<size_t>(ySample.low) * width +
                         xSample.high) * 4;
                    const auto* bottom = rgba.data() +
                        (static_cast<size_t>(ySample.high) * width +
                         xSample.low) * 4;
                    const auto* bottomRight = rgba.data() +
                        (static_cast<size_t>(ySample.high) * width +
                         xSample.high) * 4;
                    const std::uint32_t xWeight = xSample.weight;
                    const std::uint32_t yWeight = ySample.weight;
                    const std::uint32_t topWeight = 65536 - yWeight;
                    const std::uint32_t leftWeight = 65536 - xWeight;
                    for (int channel = 0; channel < 4; ++channel) {
                        const std::uint32_t topValue =
                            (static_cast<std::uint32_t>(top[channel]) * leftWeight +
                             static_cast<std::uint32_t>(topRight[channel]) * xWeight +
                             32768) >> 16;
                        const std::uint32_t bottomValue =
                            (static_cast<std::uint32_t>(bottom[channel]) * leftWeight +
                             static_cast<std::uint32_t>(bottomRight[channel]) * xWeight +
                             32768) >> 16;
                        destination[(static_cast<size_t>(x) * 4) + channel] =
                            static_cast<std::uint8_t>((topValue * topWeight +
                                                       bottomValue * yWeight +
                                                       32768) >> 16);
                    }
                }
            }
            if (!sendFrame(frame)) {
                std::fprintf(stderr, "[TURZX] frame transfer failed; disabling until next frame\n");
                close();
                std::this_thread::sleep_for(kReconnectDelay);
            } else {
                lastTransfer = std::chrono::steady_clock::now();
            }
        }
        close();
    }
};

TurzxDisplay::TurzxDisplay() : impl_(std::make_unique<Impl>()) {}
TurzxDisplay::~TurzxDisplay() { stop(); }

bool TurzxDisplay::start() {
    if (impl_->running.exchange(true)) return true;
    impl_->worker = std::thread([this] { impl_->run(); });
    return true;
}

void TurzxDisplay::stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool TurzxDisplay::available() const { return impl_->online.load(); }

void TurzxDisplay::submitRgba(const std::uint8_t* pixels, int width, int height) {
    if (!impl_->running || !pixels || width <= 0 || height <= 0) return;
    std::vector<std::uint8_t> copy(static_cast<size_t>(width) * height * 4);
    std::memcpy(copy.data(), pixels, copy.size());
    {
        std::lock_guard lock(impl_->mutex);
        impl_->pending = std::move(copy);
        impl_->pendingWidth = width;
        impl_->pendingHeight = height;
    }
    impl_->wake.notify_one();
}

} // namespace dutyon

#endif
