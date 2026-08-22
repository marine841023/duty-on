#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dutyon {

// 2.0 客户端多语言支持（移植自 1.x frontend/i18n.js 的子集）。
// 语言代码与 1.x 完全一致：zh-CN / zh-TW / en / ja / ko / fr / de / es。
class I18n {
public:
    // 支持的语言列表（code + 本地名称，用于语言子菜单）
    static const std::vector<std::pair<std::string, std::string>>& languages();

    // 遍历全部语言的全部翻译字符串（含语言本地名）。
    // 字体图集精确覆盖用：把 8 种语言实际用到的码位全部烘焙进图集，
    // 任意语言切换都不会出现缺字问号
    static void forEach(const std::function<void(const char*)>& fn);

    // 设置当前语言（不支持的语言回退 zh-CN）
    static void setLang(const std::string& code);
    static const std::string& lang();

    // 取翻译；无对应 key 时回退 zh-CN，再无则返回 key 本身
    static const char* t(const char* key);

    // 动作显示名：motion.<组>.<序号> 有翻译用翻译（如 Flick3.1 → 哈欠），
    // 否则 "组名 序号"（与 1.x getMotionName 一致）。静态缓冲，勿跨调用保存指针
    static const char* motionName(const std::string& group, int index);
};

} // namespace dutyon
