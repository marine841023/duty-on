// 语音互动文本匹配：ASR 转写 -> 唤醒/指令事件。设计要点：
//
// 1. 全量字->无声调拼音字典（pinyin.txt 资产，voiceLoadPinyinDict 加载）：
//    任何汉字都能转拼音，多音字任一读音命中即算（觉=jue/jiao）。
//    不再维护手工同音字表——v4 曾因"骂/码"漏收导致"再骂口口"唤醒失败，
//    手工表永远追不上 ASR 的同音字输出分布。
// 2. 前后鼻音归一：仅 -ing/-eng 尾 -> -in/-en（实测"惊讶"被转写成"金亚"，
//    金jin 需匹配 惊jing）。-ang 系不做归一，否则 枪qiang 会撞 欠qian
//    （哈欠）造成误触。
// 3. 唤醒词：[zai,ma,kou,kou] 拼音子序列、首尾跨度<=5，吸收"在马克扣扣"
//    （克/龙/对等中间字照常参与，只受跨度约束）等变体；另有三音节兜底
//    [ma,kou,kou] 连续（跨度=2），吸收 ASR 丢字头的"嘛口口"（实测）。
// 4. 指令词：两字拼音连续匹配；"踉跄"双模式（qiang/qiao，实测转写
//    "亮俏"）。单音节"耶"仅 final 且过滤后全是 ye、无未收录字时命中
//    （全量字典下"也"=ye 也命中——拼音匹配的自然结果，误触代价低：
//    仅多播一个动作）。
// 5. 未收录字（英文字母/标点/字典外生僻字）跳过并计数，不参与匹配。
//
// 与 hotwords.txt（ASR 热词偏置）配合：热词提高转写正确率，这里是
// 转写后的兜底模糊层，两层叠加达到"正常音量语速即可唤醒"的体验。

#include "audio/voice_match.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace dutyon {
namespace {

// 全量字典：字 -> 归一化读音集合（normPinyin 后，去重）
std::unordered_map<std::string, std::vector<std::string>> g_dict;

// 指令：名 + 两字拼音模式（踉跄两个模式任一命中）
struct CmdPattern {
    const char* name;
    const char* py1;
    const char* py2;
};
const CmdPattern kCmds[] = {
    {"发呆", "fa", "dai"},
    {"开心", "kai", "xin"},
    {"叹气", "tan", "qi"},
    {"睡觉", "shui", "jiao"},
    {"生气", "sheng", "qi"},
    {"难过", "nan", "guo"},
    {"哭泣", "ku", "qi"},
    {"喜悦", "xi", "yue"},
    {"点头", "dian", "tou"},
    {"再见", "zai", "jian"},
    {"高兴", "gao", "xing"},
    {"威胁", "wei", "xie"},
    {"肌肉", "ji", "rou"},
    {"恐惧", "kong", "ju"},
    {"惊讶", "jing", "ya"},
    {"爱心", "ai", "xin"},
    {"哈欠", "ha", "qian"},
    {"走路", "zou", "lu"},
    {"踉跄", "liang", "qiang"},
    {"踉跄", "liang", "qiao"},  // 实测转写"亮俏"
    {"摇头", "yao", "tou"},
};

// 前后鼻音归一：-ing/-eng 尾 -> -in/-en（见文件头注释 2）
std::string normPinyin(const std::string& s) {
    std::string r(s);
    const size_t n = r.size();
    if (n > 3 && r.compare(n - 3, 3, "ing") == 0) {
        r.resize(n - 1);  // 去 g
    } else if (n > 3 && r.compare(n - 3, 3, "eng") == 0) {
        r.resize(n - 1);
    }
    return r;
}

// 声母容错归一（v4.2，SenseVoice 实测系统性同部位混淆对）：
//   k↔h：哭泣(ku qi) 稳定转写为 呼气/呼泣(hu qi)——软腭塞音/擦音
//   j↔x：喜悦(xi yue) 多次转写为 几月/几悦(ji yue)——舌面音
// 仅用于指令两字匹配（连续二字提供足够特异性，已核验 10 指令无交叉冲突）
std::string normInitial(const std::string& s) {
    std::string r(s);
    if (r.size() >= 2) {
        if (r[0] == 'k') r[0] = 'h';
        else if (r[0] == 'j') r[0] = 'x';
    }
    return r;
}

// 读音集合中是否含与目标声母容错相等的音节
bool hasReadingLoose(const std::vector<std::string>& rs, const std::string& py) {
    const std::string t = normInitial(py);
    for (const auto& r : rs)
        if (normInitial(r) == t) return true;
    return false;
}

// 读音集合中是否含目标音节
bool hasReading(const std::vector<std::string>& rs, const std::string& py) {
    return std::find(rs.begin(), rs.end(), py) != rs.end();
}

// 唤醒词 kou 音节容错：ASR 实测会把 kou 近音成 苦(ku)（"再骂苦口"）。
// 仅唤醒模式放宽（4 音节本身足够特异），指令两字模式保持严格。
bool kouLike(const std::vector<std::string>& rs) {
    return hasReading(rs, "kou") || hasReading(rs, "ku");
}

// 唤醒词 ma 音节容错：ASR 实测把 ma 听成 把(ba)/往(wang)/门(men)（实测
// "再把口口/再往口口/在门口口"——语言模型向常用词 妈/把/往/门口 偏移）。
// 仅唤醒模式放宽；误触面为 [zai,ba|wang|men,kou,kou] 类短语（如"再往上
// 扣扣"），误触代价仅多应答一次，可接受。
bool maLike(const std::vector<std::string>& rs) {
    return hasReading(rs, "ma") || hasReading(rs, "ba") ||
           hasReading(rs, "wang") || hasReading(rs, "men");
}

// UTF-8 字符字节长度（容错：非法首字节当 1 字节跳过）
size_t utf8CharLen(const std::string& s, size_t i) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 转写 -> （原文片段, 归一化读音集合）序列；字典未收录字（英文/标点/
// 生僻字）跳过并计数 unknownChars（"耶"单音节匹配要求为 0）
void toPinyin(const std::string& text, std::vector<std::string>* chars,
              std::vector<std::vector<std::string>>* py, size_t* unknownChars) {
    size_t i = 0;
    while (i < text.size()) {
        const size_t len = utf8CharLen(text, i);
        if (i + len > text.size()) break;
        const std::string ch = text.substr(i, len);
        const auto it = g_dict.find(ch);
        if (it != g_dict.end()) {
            chars->push_back(ch);
            py->push_back(it->second);
        } else {
            ++*unknownChars;
        }
        i += len;
    }
}

}  // namespace

size_t voiceLoadPinyinDict(const std::string& path) {
    g_dict.clear();
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        // 格式："字 py1 [py2 ...]"；异常行（空/无空格）跳过
        const std::string s(line);
        const size_t sp = s.find(' ');
        if (sp == std::string::npos || sp == 0) continue;
        const std::string ch = s.substr(0, sp);
        if (utf8CharLen(ch, 0) != ch.size()) continue;  // 首 token 须是单字
        std::vector<std::string> rs;
        size_t p = sp + 1;
        while (p < s.size()) {
            size_t q = std::min(s.find(' ', p), s.size());
            // 行尾换行随最后一片读入，这里统一剥掉
            std::string tok = s.substr(p, q - p);
            while (!tok.empty() && (tok.back() == '\n' || tok.back() == '\r'))
                tok.pop_back();
            if (!tok.empty()) {
                const std::string np = normPinyin(tok);
                if (!hasReading(rs, np)) rs.push_back(np);
            }
            if (q == s.size()) break;
            p = q + 1;
        }
        if (!rs.empty()) g_dict[ch] = std::move(rs);
    }
    std::fclose(f);
    return g_dict.size();
}

std::vector<VoiceAsrUpdate> voiceParseAsrUpdates(const std::string& rawLine) {
    std::vector<VoiceAsrUpdate> out;
    // 行完整性：无结尾 \n（fgets 缓冲截断/子进程被杀残留）则无 final
    const bool lineComplete = !rawLine.empty() && rawLine.back() == '\n';
    // 1) 剥 ANSI CSI 序列（\x1b[2K 清行码），按 \r 切片
    std::vector<std::string> frags;
    std::string cur;
    for (size_t i = 0; i < rawLine.size(); ++i) {
        const char c = rawLine[i];
        if (c == '\x1b' && i + 1 < rawLine.size() && rawLine[i + 1] == '[') {
            i += 2;  // 跳过 ESC [
            while (i < rawLine.size()) {
                const char f = rawLine[i];
                if ((f >= 'A' && f <= 'Z') || (f >= 'a' && f <= 'z')) break;
                ++i;  // CSI 参数字节
            }
        } else if (c == '\r') {
            frags.push_back(cur);
            cur.clear();
        } else if (c != '\n') {
            cur += c;
        }
    }
    frags.push_back(cur);
    // 2) 逐片解析 "<段号>:<文本>"；最后一片且行完整 => final
    for (size_t i = 0; i < frags.size(); ++i) {
        const std::string& f = frags[i];
        if (f.empty()) continue;
        char* e = nullptr;
        const long idx = std::strtol(f.c_str(), &e, 10);
        (void)idx;  // 段号仅作格式校验（段切换由 isFinal 表达）
        if (e == f.c_str() || *e != ':') continue;
        out.push_back({std::string(e + 1), i + 1 == frags.size() && lineComplete});
    }
    return out;
}

std::string voiceMatchText(const std::string& text, bool allowSingleSyllable,
                           std::string* matchedChars) {
    if (g_dict.empty()) return {};
    // SenseVoice 跨语言先验：唤醒词"扣扣"常被转写成英文（小写归一后按
    // 长到短替换，防止长词被短特例拆开）。实测变体：
    //   coco/koko/kuku（2026-10-06）；qq / q q（2026-10-07，字母名 kiü
    //   近听，"在吗 qq"）；混合词"扣co"；两字母组合 co/ko/ku 单独出现
    //   近听为"扣"（替换成汉字才能进拼音字典，英文串只会被跳过）
    std::string normalized;
    normalized.reserve(text.size());
    for (char c : text) normalized.push_back(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    const std::pair<const char*, const char*> kEnNorm[] = {
        {"coco", "扣扣"}, {"koko", "扣扣"}, {"kuku", "扣扣"},
        {"qq", "扣扣"},   {"q q", "扣扣"},
        {"co", "扣"},     {"ko", "扣"},     {"ku", "扣"},
    };
    for (const auto& kv : kEnNorm) {
        const std::string from = kv.first;
        const std::string to = kv.second;
        for (size_t pos = normalized.find(from); pos != std::string::npos;
             pos = normalized.find(from, pos + to.size()))
            normalized.replace(pos, from.size(), to);
    }
    std::vector<std::string> chars;
    std::vector<std::vector<std::string>> rs;
    size_t unknown = 0;
    toPinyin(normalized, &chars, &rs, &unknown);
    const size_t n = rs.size();
    if (n == 0) return {};

    // 唤醒词：[zai,ma,kou,kou] 子序列，首尾跨度 <= 5（p3-p0<=4，容忍
    // 中间多一个音节；全量字典下所有字都参与，跨度即原文字数距离）；
    // kou 位置接受近音 ku（kouLike，实测"再骂苦口"）
    for (size_t a = 0; a + 3 < n; ++a) {
        if (!hasReading(rs[a], "zai")) continue;
        for (size_t b = a + 1; b < n && b <= a + 4; ++b) {
            if (!hasReading(rs[b], "ma")) continue;
            for (size_t c = b + 1; c < n && c <= a + 4; ++c) {
                if (!kouLike(rs[c])) continue;
                for (size_t d = c + 1; d < n && d <= a + 4; ++d) {
                    if (!kouLike(rs[d]) || d - a > 4) continue;
                    if (matchedChars)
                        *matchedChars = chars[a] + chars[b] + chars[c] + chars[d];
                    return "DutyOn";
                }
            }
        }
    }

    // 唤醒兜底：[ma,kou,kou] 三音节连续（ASR 丢字头"在"，实测"嘛口口"）。
    // 要求过滤后严格相邻（跨度=2），"马化腾的扣扣"（中间隔字）不误触
    for (size_t i = 0; i + 2 < n; ++i) {
        if (hasReading(rs[i], "ma") && kouLike(rs[i + 1]) && kouLike(rs[i + 2])) {
            if (matchedChars)
                *matchedChars = chars[i] + chars[i + 1] + chars[i + 2];
            return "DutyOn";
        }
    }

    // 指令：两字拼音连续匹配（多音字任一读音命中；v4.2 起声母容错
    // k↔h/j↔x，吸收 SenseVoice 的 哭泣→呼气、喜悦→几月 系统性混淆）
    for (const auto& cmd : kCmds) {
        const std::string p1 = normPinyin(cmd.py1);
        const std::string p2 = normPinyin(cmd.py2);
        for (size_t i = 0; i + 1 < n; ++i) {
            if (hasReadingLoose(rs[i], p1) && hasReadingLoose(rs[i + 1], p2)) {
                if (matchedChars) *matchedChars = chars[i] + chars[i + 1];
                return cmd.name;
            }
        }
    }

    // 单音节"耶"：仅 final 放开；要求过滤后全是 ye 且无未收录字（"夜晚"
    // 含 wan 不满足），1~3 个（"耶耶"也算）。全量字典下"也"=ye 同样命中。
    if (allowSingleSyllable && unknown == 0 && n <= 3) {
        bool allYe = true;
        for (const auto& r : rs)
            if (!hasReading(r, "ye")) { allYe = false; break; }
        if (allYe) {
            if (matchedChars) *matchedChars = chars[0];
            return "耶";
        }
    }

    return {};
}

}  // namespace dutyon
