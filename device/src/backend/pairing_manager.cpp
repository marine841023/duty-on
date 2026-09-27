// 配对管理器实现（仅 PC 端）—— pending/paired 状态 + config.json 持久化。

#ifdef _WIN32

#include "backend/pairing_manager.h"

#include <chrono>
#include <cstdio>
#include <random>

#include "config/user_config.h"  // loadPairedDevices / savePairedDevices

namespace dutyon::backend {

namespace {

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 待配对请求有效期：设备每 3s 重试一次 pair-request，120s 无刷新即视为
// 设备已离开/放弃，从列表清除（避免 UI 堆积陈旧条目）
const long long kPendingTtlMs = 120000;

} // namespace

PairingManager::PairingManager() {
    paired_ = dutyon::UserConfigStore::loadPairedDevices();
    if (!paired_.empty())
        printf("[Pairing] loaded %zu paired device(s)\n", paired_.size());
}

std::string PairingManager::generateToken() {
    // 32 位十六进制随机令牌（128 bit 熵）：设备持有即视为已配对
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string t;
    t.reserve(32);
    for (int i = 0; i < 32; i++) t += hex[rng() & 0xF];
    return t;
}

std::string PairingManager::handleRequest(const std::string& device_id,
                                          const std::string& code,
                                          const std::string& ip,
                                          std::string* out_token) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (auto it = paired_.find(device_id); it != paired_.end()) {
        // 已配对（含用户刚在 UI 确认）：回其 token，设备据此结束握手
        if (out_token) *out_token = it->second;
        pending_.erase(device_id);
        return "paired";
    }
    // 未配对：记录/刷新 pending，等用户在 PC 输码确认
    PendingPair& p = pending_[device_id];
    p.device_id = device_id;
    p.code = code;
    p.ip = ip;
    p.last_seen_ms = nowMs();
    if (out_token) out_token->clear();
    return "pending";
}

bool PairingManager::isPaired(const std::string& device_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return paired_.find(device_id) != paired_.end();
}

bool PairingManager::validateToken(const std::string& token) const {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& kv : paired_)
        if (kv.second == token) return true;
    return false;
}

std::vector<PendingPair> PairingManager::pendingList() {
    std::lock_guard<std::mutex> lk(mtx_);
    const long long now = nowMs();
    // 顺带老化清理：超时未刷新的请求移除
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second.last_seen_ms > kPendingTtlMs)
            it = pending_.erase(it);
        else
            ++it;
    }
    std::vector<PendingPair> out;
    out.reserve(pending_.size());
    for (const auto& kv : pending_) out.push_back(kv.second);
    return out;
}

std::vector<std::string> PairingManager::pairedList() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    out.reserve(paired_.size());
    for (const auto& kv : paired_) out.push_back(kv.first);
    return out;
}

std::string PairingManager::confirmByCode(const std::string& code) {
    if (code.empty()) return std::string();
    std::lock_guard<std::mutex> lk(mtx_);
    const long long now = nowMs();
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        // 忽略超时请求；code 精确匹配（设备屏幕显示的 6 位码）
        if (now - it->second.last_seen_ms > kPendingTtlMs) continue;
        if (it->second.code != code) continue;
        const std::string device_id = it->second.device_id;
        const std::string token = generateToken();
        paired_[device_id] = token;
        pending_.erase(it);
        persistLocked();
        printf("[Pairing] paired device %s\n", device_id.c_str());
        return device_id;
    }
    return std::string();
}

bool PairingManager::unpair(const std::string& device_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (paired_.erase(device_id) == 0) return false;
    persistLocked();
    printf("[Pairing] unpaired device %s\n", device_id.c_str());
    return true;
}

void PairingManager::persistLocked() {
    dutyon::UserConfigStore::savePairedDevices(paired_);
}

} // namespace dutyon::backend

#endif // _WIN32
