#pragma once

// ---------------------------------------------------------------------------
// 电子相框「指定文件夹」照片播放器（仅设备端 ARM Linux）。
//
// 需求约束（用户）：
//   - 不要一口气把所有照片同步到设备；播完当前这张再加载下一张；
//   - 随时保证设备里的图片不要太多。
//
// 实现策略（逐张流式，零落盘）：
//   - 任意时刻设备侧最多驻留：当前显示的一张（已解码 RGBA + GL 纹理）
//     + 后台在途的一张编码字节。照片从不写磁盘。
//   - 网络取字节放后台线程（ApiClient::fetchFramePhoto），渲染线程零阻塞；
//     解码 + GL 上传在渲染线程（GL 上下文只在此线程可用）。
//   - 5 秒一张；随机顺序由 PC 端抽取（见后端 GET /api/frame/photo）。
//   - 显示第 N 张的同时后台已在取第 N+1 张；到点若下一张未就绪则继续显示
//     当前（不留白），就绪后再切。
//
// 渲染：GLES2 单张纹理四边形，cover 适配（等比铺满、居中裁边，溢出部分由
// NDC 裁剪）。与 GifSprite 设备版同源 shader，互不干扰 GL 状态。
// ---------------------------------------------------------------------------

#ifndef _WIN32

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace dutyon {

class PhotoPlayer {
public:
    PhotoPlayer();
    ~PhotoPlayer();

    // 注入取字节回调（内部调 ApiClient::fetchFramePhoto；在后台线程执行）。
    // 返回 false = 本次取失败（PC 离线 / 无照片），后台稍后自动重试。
    void setFetcher(std::function<bool(std::vector<unsigned char>&)> f);

    // 启动/停止后台线程。start 幂等（已在跑则忽略）；stop 会 join 线程并
    // 在渲染线程删除 GL 纹理（须在上下文当前时调用，即主循环内）。
    void start();
    void stop();

    // 渲染线程每帧调用：推进计时；到点（或首张）且有就绪照片 -> 解码 + 上传
    // + 切换显示，并请求后台取下一张。
    void update(float delta_seconds);

    // 画到给定 GL 视口（原点左下，像素）。无当前图时不动作。
    void render(int vp_x, int vp_y, int vp_w, int vp_h);

    // 是否已有可显示的照片（用于决定是否隐藏角色/时钟）。
    bool hasImage() const { return have_cur_; }

private:
    void workerMain();
    void ensureProgram();
    // 渲染线程：把 mailbox 中就绪的编码字节解码并上传为当前纹理。
    bool takePendingToTexture();

    // ---- 后台线程 -> 渲染线程 单槽 mailbox（一张待解码的编码字节）----
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::mutex q_mtx_;
    std::condition_variable q_cv_;
    std::vector<unsigned char> pending_;  // 待渲染线程消费的编码字节
    bool pending_ready_ = false;          // pending_ 已备好可取
    bool want_fetch_ = false;             // 请求后台取下一张（受 q_mtx_ 保护）
    std::function<bool(std::vector<unsigned char>&)> fetcher_;

    // ---- 渲染线程侧：当前纹理 + GL + 计时 ----
    unsigned int tex_ = 0;
    unsigned int program_ = 0;
    int tex_w_ = 0, tex_h_ = 0;
    bool have_cur_ = false;
    float timer_ = 0.f;
    int vp_x_ = 0, vp_y_ = 0, vp_w_ = 0, vp_h_ = 0;
};

}  // namespace dutyon

#endif  // !_WIN32
