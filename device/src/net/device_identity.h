#pragma once

// 设备身份（仅设备端）：持久化 device_id / pair_code / 配对 token 到
// ~/.dutyon/device.json（首次启动随机生成，之后复用）。
//
//   device_id  16 位十六进制随机串，设备唯一标识；AP SSID 取其后 4 位
//              （DutyOn-<后4位>），配对握手用它区分不同设备。
//   pair_code  6 位数字，屏幕显示给用户，用户在 PC 端输入以完成配对。
//   ap_pass    8 位随机数字，AP 热点口令（WPA2 最短 8 位、数字键盘好输）。
//   token      配对成功后 PC 签发的令牌，空 = 未配对；持久化后重启免再配。
//
// 单例：进程内首次 instance() 读文件（无则生成并落盘），之后走内存缓存。
#ifndef _WIN32

#include <string>

namespace dutyon {

class DeviceIdentity {
public:
    static DeviceIdentity& instance();

    const std::string& deviceId() const { return device_id_; }
    const std::string& pairCode() const { return pair_code_; }
    const std::string& apPass() const { return ap_pass_; }
    std::string token() const { return token_; }
    bool paired() const { return !token_.empty(); }

    // AP 热点名后缀（device_id 后 4 位，大写）
    std::string shortSuffix() const;

    // 配对成功后保存 token（持久化）；传空串 = 解除配对
    void setToken(const std::string& token);

private:
    DeviceIdentity();
    void load();
    void save() const;

    std::string device_id_;
    std::string pair_code_;
    std::string ap_pass_;
    std::string token_;
};

} // namespace dutyon

#endif // !_WIN32
