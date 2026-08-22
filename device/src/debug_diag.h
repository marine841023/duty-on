#pragma once

// 环境变量门控的诊断工具（DUTYON_FT_PROBE / DUTYON_MEM）。默认零开销，
// 发布版本无副作用；仅设置对应环境变量时输出诊断日志。
//
// 从 main.cpp 抽出：崩溃诊断/内存定位这类工具不应与主流程代码混排。
namespace dutyon {

// DUTYON_FT_PROBE=1：绕开 ImGui 直接验证 FreeType FT_New_Memory_Face +
// 光栅化是否复制字体数据（定位字体文件驻留内存的根因）
void FtProbe();

// DUTYON_MEM=1：VirtualQuery 遍历已提交私有内存按 AllocationBase 聚合，
// HeapWalk 统计进程堆实际 in-use（定位工作集/私有内存大头：驱动 vs 堆 vs 栈）
void DumpMemoryComposition();

}  // namespace dutyon
