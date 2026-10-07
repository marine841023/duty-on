#pragma once

// 语音互动文本匹配（仅设备端编译；无平台依赖的纯 C++）。
//
// 流式 ASR（sherpa-onnx zh-14M）输出的转写文本 -> 语音事件：
//   "DutyOn"  唤醒词"在吗扣扣"命中（拼音子序列模糊匹配）
//   指令名    动作指令命中（两字连续拼音匹配，如"开心"）
//   空串      未命中
//
// 匹配基础是全量字->无声调拼音字典（pinyin.txt 资产，26704 字/5874 多音字，
// pypinyin 生成，见 .userdata/gen-pinyin-dict.py）：任何转写字都能转拼音，
// 多音字任一读音命中即算——不维护手工同音字表（v4 曾因"骂/码"漏收导致
// "再骂口口"唤醒失败）。详细设计见 voice_match.cpp 文件头注释。

#include <string>
#include <vector>

namespace dutyon {

// 加载全量拼音字典（每行 "字 拼音1 [拼音2 ...]"，多音字空格分隔）。
// 返回收录字数；0 = 加载失败（文件缺失/格式错）。重复调用覆盖旧表。
size_t voiceLoadPinyinDict(const std::string& path);

// 匹配 ASR 转写文本。
//   text                UTF-8 转写（partial 或 final 均可）
//   allowSingleSyllable 单音节指令（"耶"）仅在 final 结果上放开，防 partial
//                       前缀误触（如"夜晚"的 partial"夜"）
//   matchedChars        出参（可空）：命中时回写命中字片段，供调用方做
//                       流式去重（后续 partial 仍包含该片段则不再触发）
// 返回 "DutyOn"/指令中文名/空串。未加载字典恒返回空串。
std::string voiceMatchText(const std::string& text, bool allowSingleSyllable,
                           std::string* matchedChars);

// sherpa-onnx-alsa 实时输出的一条更新（一个语音段的一次转写刷新）
struct VoiceAsrUpdate {
    std::string text;  // 转写文本（可为空串）
    bool isFinal;      // 段结束（端点检测静音收尾，之后开新段）
};

// 解析 sherpa-onnx-alsa 的一条原始输出行（fgets 一次读到的内容）为更新
// 序列。协议实测（v1.12.22，与文件模式二进制的 JSON 输出不同！）：
//   - 同一语音段的所有 partial 刷新在行内以 \r 分隔，段结束补 \n：
//       "\r0:什\x1b[2K\r\r0:什么\n"  ->  partial"什"，final"什么"
//   - 片段格式 "<段号>:<文本>"；启动 banner（模型配置/"Started! Please
//     speak"）无 N: 前缀，自然忽略
//   - 行尾一片为 final（无 \n 则全部是 partial——子进程被杀时的残留）
std::vector<VoiceAsrUpdate> voiceParseAsrUpdates(const std::string& rawLine);

}  // namespace dutyon
