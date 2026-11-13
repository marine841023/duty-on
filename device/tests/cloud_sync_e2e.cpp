// 云同步端到端测试：真实的 CloudClient 对着一个真实的服务端（本地起的
// dutyon-cloud），验证"网页改了角色，桌宠能跟上"的双向对账。
//
// 运行：先在 server/ 下 `go run . --addr 127.0.0.1:8799 --data <临时目录>`，
// 再执行 device\tests\run-cloud-sync-e2e.cmd http://127.0.0.1:8799
// 测试用临时的 USERPROFILE，不碰真实用户数据；每次注册一个新账号。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "cloud/cloud_client.h"
#include "cloud/cloud_transport.h"

using namespace dutyon;
using json = nlohmann::json;
namespace fs = std::filesystem;

static void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
static void writeFile(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o << body;
}
static std::string readFile(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(i), std::istreambuf_iterator<char>());
}
template <class F>
static void waitFor(F done, const char* what) {
    for (int i = 0; i < 300; ++i) {
 if (done()) return;
 std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error(what);
}

int main(int argc, char** argv) {
    try {
        const std::string origin = argc > 1 ? argv[1] : "http://127.0.0.1:8799";
        const std::string user = "sync_e2e_" + std::to_string(GetTickCount64());
        const fs::path root = fs::temp_directory_path() / ("dutyon-sync-e2e-" + user);
        fs::create_directories(root);
 _putenv_s("USERPROFILE", root.string().c_str());
 const fs::path dir = root / ".dutyon";
 const fs::path cfg_path = dir / "config.json";
 auto anim = [&](const char* name) { return dir / "animations" / name; };
 auto loadCfg = [&] { return json::parse(readFile(cfg_path)); };
        auto saveCfg = [&](const json& j) { writeFile(cfg_path, j.dump(2)); };

        // ---- 1. 本机已有一个角色，首次同步上传 ----
 writeFile(anim("char_1_working_100.gif"), "anim-a");
        saveCfg(json::parse(R"({"language":"zh-CN","customCharacters":[
     {"id":"char_1","name":"A","sleeping":"","working":"char_1_working_100.gif","alert":""}]})"));
 CloudClient client;
        require(client.registerAccount(origin, user, "DutyOn-test-only-2026").empty(), "register");
        const std::string token = loadCfg()["cloudToken"].get<std::string>();
 CloudTransport web(origin);  // 扮演网页
 const httplib::Headers auth = httplib::Headers({{"Authorization", "Bearer " + token}});
 auto synced = [&] { return client.syncHint().find(u8"已同步") == 0; };
        auto cloudConfig = [&] {
     auto r = web.Get("/api/sync/config", auth);
     require(r && r->status == 200, "get config");
     return json::parse(r->body);
        };
 auto cloudHas = [&](const std::string& path) {
     auto r = web.Get("/api/sync/manifest", auth);
     require(r && r->status == 200, "manifest");
     for (const auto& e : json::parse(r->body))
  if (e["path"] == path) return true;
     return false;
        };
 auto webPutConfig = [&](json cfg, const std::string& rev) {
     auto h = auth;
     h.emplace("If-Match", "\"" + rev + "\"");
     auto r = web.Put("/api/sync/config", h, cfg.dump(), "application/json");
     require(r && r->status == 200, "web config save");
 };
        client.startWorker();
 waitFor(synced, "initial sync");
        require(cloudHas("animations/char_1_working_100.gif"), "local file uploaded");
        require(cloudConfig()["config"]["customCharacters"][0]["name"] == "A", "local config uploaded");

        // ---- 2. 网页上：换掉 A 的动画、新建角色 B、删掉旧动画 ----
        require(web.Put("/api/sync/file?path=animations/char_1_working_200.gif", auth, "anim-b",
   "application/octet-stream")->status == 200, "web upload");
        json remote = cloudConfig();
 remote["config"]["customCharacters"][0]["working"] = "char_1_working_200.gif";
 remote["config"]["customCharacters"].push_back(
     {{"id", "char_2"}, {"name", "B"}, {"sleeping", ""}, {"working", ""}, {"alert", ""}});
        webPutConfig(remote["config"], remote["rev"].get<std::string>());
 require(web.Delete("/api/sync/file?path=animations/char_1_working_100.gif", auth)->status == 200,
  "web delete");

        // ---- 3. 桌宠下一轮同步：自动应用，不把旧文件传回去 ----
        client.syncNow();
        waitFor([&] { return client.restoreDone(); }, "cloud edit applied");
        client.consumeRestore();
        json local = loadCfg();
        require(local["customCharacters"][0]["working"] == "char_1_working_200.gif", "animation switched locally");
 require(local["customCharacters"].size() == 2 && local["customCharacters"][1]["name"] == "B",
  "new character arrived");
        require(readFile(anim("char_1_working_200.gif")) == "anim-b", "new animation downloaded");
        require(!fs::exists(anim("char_1_working_100.gif")), "deleted-on-web file left the animations folder");
 require(readFile(dir / "cloud-trash" / "animations_char_1_working_100.gif") == "anim-a",
  "deleted-on-web file kept in cloud-trash");
 client.syncNow();
 waitFor(synced, "resync after apply");
 require(!cloudHas("animations/char_1_working_100.gif"), "deleted file must not be re-uploaded");
 require(local.value("cloudToken", std::string()) == token, "local-only fields survive");

        // ---- 4. 只有本机改了：照常上传 ----
 local["customCharacters"][0]["name"] = "A2";
        saveCfg(local);
 client.syncNow();
 waitFor([&] { return cloudConfig()["config"]["customCharacters"][0]["name"] == "A2"; }, "local edit pushed");

        // ---- 5. 两边都改了：不覆盖任何一边；再点一次立即同步 = 以本机为准 ----
        waitFor(synced, "settle");
 remote = cloudConfig();
 remote["config"]["customCharacters"][1]["name"] = "B-web";
        webPutConfig(remote["config"], remote["rev"].get<std::string>());
 local = loadCfg();
 local["customCharacters"][1]["name"] = "B-local";
        saveCfg(local);
        client.syncNow();
        waitFor([&] { return client.syncHint().find(u8"两边都改过") != std::string::npos ||
          client.syncHint().find(u8"都改过") != std::string::npos; },
  "conflict reported");
 require(cloudConfig()["config"]["customCharacters"][1]["name"] == "B-web", "cloud not overwritten on conflict");
        require(loadCfg()["customCharacters"][1]["name"] == "B-local", "local not overwritten on conflict");
 client.syncNow();
 waitFor([&] { return cloudConfig()["config"]["customCharacters"][1]["name"] == "B-local"; },
  "second sync-now pushes local");

 // ---- 6. 本机删掉一个不再使用的文件：云端跟着删 ----
        waitFor(synced, "settle 2");
        local = loadCfg();
        local["customCharacters"][0]["working"] = "";
        saveCfg(local);
        fs::remove(anim("char_1_working_200.gif"));
        client.syncNow();
        waitFor([&] { return !cloudHas("animations/char_1_working_200.gif"); }, "local delete propagated");

 // ---- 7. 仍被角色引用的文件在网页上被删：不动本机，传回去 ----
 writeFile(anim("char_2_alert_300.gif"), "anim-c");
        local = loadCfg();
        local["customCharacters"][1]["alert"] = "char_2_alert_300.gif";
        saveCfg(local);
 client.syncNow();
 waitFor([&] { return cloudHas("animations/char_2_alert_300.gif"); }, "referenced file uploaded");
        waitFor(synced, "settle 3");
        require(web.Delete("/api/sync/file?path=animations/char_2_alert_300.gif", auth)->status == 200,
  "web delete referenced");
        client.syncNow();
 waitFor([&] { return cloudHas("animations/char_2_alert_300.gif"); }, "referenced file restored to cloud");
 require(fs::exists(anim("char_2_alert_300.gif")), "referenced file stays on disk");

        client.logout();
 std::error_code ec;
        fs::remove_all(root, ec);
        std::cout << "Cloud sync E2E passed: upload, apply web edits, trash web-deleted files, push local "
"edits, conflict protection, force push, propagate local deletes, keep referenced files\n";
 return 0;
    } catch (const std::exception& e) {
 std::cerr << "FAILED: " << e.what() << "\n";
 return 1;
    }
}
