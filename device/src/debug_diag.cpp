// 环境变量门控的诊断工具实现（见 debug_diag.h）。仅 Windows（_WIN32）。
#include "debug_diag.h"

#ifdef _WIN32

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <psapi.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#pragma comment(lib, "psapi.lib")

namespace dutyon {

// 堆内已分配字节（HeapWalk 总和）
static size_t HeapInUseBytes() {
    size_t inuse = 0;
    DWORD nheaps = GetProcessHeaps(0, nullptr);
    if (nheaps == 0) return 0;
    std::vector<HANDLE> heaps(nheaps);
    nheaps = GetProcessHeaps((DWORD)heaps.size(), heaps.data());
    for (DWORD h = 0; h < nheaps; h++) {
        PROCESS_HEAP_ENTRY e{};
        if (!HeapLock(heaps[h])) continue;
        e.lpData = nullptr;
        while (HeapWalk(heaps[h], &e)) {
            if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY)
                inuse += e.cbData + e.cbOverhead;
        }
        HeapUnlock(heaps[h]);
    }
    return inuse;
}

void FtProbe() {
    printf("[FtProbe] heap in-use: %.2f MB (baseline)\n",
           HeapInUseBytes() / 1048576.0);
    FILE* f = fopen("C:\\Windows\\Fonts\\msyh.ttc", "rb");
    if (!f) { printf("[FtProbe] open msyh.ttc failed\n"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = (uint8_t*)malloc(sz);
    if (!data || fread(data, 1, sz, f) != (size_t)sz) { fclose(f); return; }
    fclose(f);
    printf("[FtProbe] heap in-use: %.2f MB (after read %ld bytes)\n",
           HeapInUseBytes() / 1048576.0, sz);

    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib)) { free(data); return; }
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(lib, data, (FT_Long)sz, 0, &face)) {
        printf("[FtProbe] FT_New_Memory_Face failed\n");
        FT_Done_FreeType(lib); free(data); return;
    }
    printf("[FtProbe] heap in-use: %.2f MB (after NewMemoryFace)\n",
           HeapInUseBytes() / 1048576.0);

    FT_Set_Pixel_Sizes(face, 0, 19);
    int rendered = 0;
    for (uint32_t cp = 0x4E00; cp < 0x4E00 + 500 && rendered < 500; cp++) {
        FT_UInt gi = FT_Get_Char_Index(face, cp);
        if (!gi) continue;
        if (FT_Load_Glyph(face, gi, FT_LOAD_FORCE_AUTOHINT)) continue;
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) continue;
        rendered++;
    }
    printf("[FtProbe] heap in-use: %.2f MB (after render %d glyphs)\n",
           HeapInUseBytes() / 1048576.0, rendered);

    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    free(data);
    printf("[FtProbe] heap in-use: %.2f MB (after DoneFace/DoneFT/free)\n",
           HeapInUseBytes() / 1048576.0);
}

void DumpMemoryComposition() {
    // 已加载模块基址 -> 文件名（用于归属私有内存块）
    std::vector<std::pair<uintptr_t, std::string>> mods;
    HMODULE hmods[1024];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), hmods, sizeof(hmods), &needed)) {
        const int n = (int)(needed / sizeof(HMODULE));
        for (int i = 0; i < n && i < 1024; i++) {
            char path[MAX_PATH] = {0};
            if (GetModuleFileNameA(hmods[i], path, MAX_PATH)) {
                const char* base = strrchr(path, '\\');
                mods.emplace_back((uintptr_t)hmods[i],
                                  base ? base + 1 : path);
            }
        }
        std::sort(mods.begin(), mods.end());
    }

    // AllocationBase -> 聚合字节 + 归属名
    struct Agg { size_t bytes = 0; std::string name; };
    std::map<uintptr_t, Agg> by_alloc;
    size_t total_private = 0, image_commit = 0, mapped_commit = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    while (addr < 0x7FFFFFFFFFFF) {
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT) {
            if (mbi.Type == MEM_PRIVATE) {
                total_private += mbi.RegionSize;
                auto& a = by_alloc[(uintptr_t)mbi.AllocationBase];
                a.bytes += mbi.RegionSize;
            } else if (mbi.Type == MEM_IMAGE) {
                image_commit += mbi.RegionSize;
            } else {
                mapped_commit += mbi.RegionSize;
            }
        }
        const uintptr_t next = addr + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }

    printf("[MemDiag] private=%zu MB  image=%zu MB  mapped=%zu MB\n",
           total_private >> 20, image_commit >> 20, mapped_commit >> 20);

    // 归属名：精确匹配模块基址，否则找不大于它的最大模块（"under" 前缀，
    // 表示该私有块分配自该模块的代码/驱动运行时）
    std::vector<std::pair<size_t, std::string>> rows;
    for (auto& [base, agg] : by_alloc) {
        std::string name;
        auto it = std::lower_bound(mods.begin(), mods.end(), base,
                                   [](const auto& m, uintptr_t v) { return m.first < v; });
        if (it != mods.end() && it->first == base) {
            name = it->second;
        } else if (it != mods.begin()) {
            --it;
            name = "under " + it->second;
        } else {
            name = "(heap/virtual)";
        }
        rows.emplace_back(agg.bytes, name);
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < rows.size() && i < 14; i++) {
        printf("[MemDiag]   %8.2f MB  %s\n", rows[i].first / 1048576.0,
               rows[i].second.c_str());
    }

    // 进程堆实际使用（HeapWalk；HeapDestroy 语义外的大块也统计）
    DWORD nheaps = GetProcessHeaps(0, nullptr);
    if (nheaps > 0) {
        std::vector<HANDLE> heaps(nheaps);
        nheaps = GetProcessHeaps((DWORD)heaps.size(), heaps.data());
        size_t heap_commit = 0, heap_inuse = 0;
        std::vector<std::pair<size_t, uintptr_t>> big_blocks;
        for (DWORD h = 0; h < nheaps; h++) {
            PROCESS_HEAP_ENTRY e{};
            HeapLock(heaps[h]);
            e.lpData = nullptr;
            while (HeapWalk(heaps[h], &e)) {
                if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                    heap_inuse += e.cbData + e.cbOverhead;
                    if (e.cbData >= 2u * 1024u * 1024u)
                        big_blocks.emplace_back(e.cbData,
                                                (uintptr_t)e.lpData);
                } else if (e.wFlags & PROCESS_HEAP_REGION) {
                    heap_commit += (size_t)e.Region.dwCommittedSize;
                }
            }
            HeapUnlock(heaps[h]);
        }
        printf("[MemDiag] heaps: in-use=%zu MB committed=%zu MB\n",
               heap_inuse >> 20, heap_commit >> 20);
        std::sort(big_blocks.begin(), big_blocks.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        printf("[MemDiag] blocks >= 2MB: %zu\n", big_blocks.size());
        for (size_t i = 0; i < big_blocks.size() && i < 16; i++) {
            const unsigned char* p = (const unsigned char*)big_blocks[i].second;
            printf("[MemDiag]   %8.2f MB @ 0x%llx  head: %02x%02x%02x%02x %02x%02x%02x%02x\n",
                   big_blocks[i].first / 1048576.0,
                   (unsigned long long)big_blocks[i].second,
                   p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }
}

}  // namespace dutyon

#endif  // _WIN32
