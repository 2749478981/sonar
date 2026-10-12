// ===========================================================================
//  SonarEfx.cpp —— 特效捕获实现（见 SonarEfx.h 的设计说明）
// ===========================================================================
#include "SonarEfx.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

#include "MinHook.h"
#include "SonarEfxEpv.h"   // .epv3 解析：用游戏目录里真实的表建索引

namespace sonarefx {
namespace {

// ---------------------------------------------------------------------------
//  常量
// ---------------------------------------------------------------------------
// Effect:Ctor 入口的 5 字节跳板：jmp rel32（目标 = 真身）
//   来源：vfx-forge docs/index.md（实测：入口 RVA 0x0AE0D70 → 真身 0x0ADD670）
const unsigned char kSigEffectCtor[5] = { 0xE9, 0xFB, 0xC8, 0xFF, 0xFF };

const wchar_t* kLogName = L"WeaponSoundEnhance_efx.log";
const char*    kIndexName = "WseEfxIndex.txt";

// 环形缓冲：hook（游戏线程）写、Flush（主线程）读
const unsigned kRingSize = 1024;
struct RawCall { std::uint32_t group; std::uint32_t rec; std::uint64_t tick; std::uint64_t a1; std::uint64_t a2; std::uint32_t via; };
RawCall g_ring[kRingSize];
std::atomic<std::uint32_t> g_head{ 0 };
std::atomic<std::uint32_t> g_tail{ 0 };

// ---------------------------------------------------------------------------
//  索引：WseEfxIndex.txt
//    #TABLE|<表名>|<表路径>
//    <表名>\t<组号>\t<组内下标>\t<recordID>\t<资源路径>
// ---------------------------------------------------------------------------
struct Entry {
    std::string table;
    std::string path;
    int         recId = -1;
    std::uint32_t group = 0;
    std::uint32_t rec = 0;
};
std::unordered_map<std::uint64_t, std::vector<int>> g_lookup;   // key=(group<<32|rec) → Entry 下标
std::vector<Entry> g_entries;
std::vector<std::string> g_tables;
std::size_t   g_skipped = 0;   // 索引里被跳过的非 hm_wp* 表数（自检显示用）
bool          g_logAll = true; // 诊断：记录所有原始调用（每个 (组,记录) 只记一次）
std::unordered_map<std::uint64_t, int> g_rawSeen;   // 已记录的原始 (组,记录)
std::uint64_t g_lastStats = 0;

std::wstring  g_dir;
std::wstring  g_gameRoot;      // 游戏根目录（用于扫 nativePC 下真实的 .epv3）
std::wstring  g_logPath;
bool          g_hooked = false;
std::uintptr_t g_hookAddr = 0;
std::string   g_status = "未初始化";
std::string   g_indexSummary;   // 索引摘要（表数/条数/覆盖的磁盘文件）
std::string   g_lastCand;      // 最近一次捕获的候选表名（同一 (组,记录) 可能存在于多张表）
std::string   g_tablePref[14];   // 每把武器的特效表偏好（ini: EfxTablePref=3=hm_wp03_25）
bool          g_enabled = true;
int         (*g_weaponFn)() = nullptr;
std::string   g_lastLine;      // 去重：上一条 (表,组,记录)
std::uint64_t g_lastTick = 0;
Stats         g_stats;
std::uint32_t g_seq = 0;

using EffectCtorFn = void* (__fastcall*)(void* self, int group, int record);
EffectCtorFn g_orig = nullptr;

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 只保留文件名（日志里别写本地路径）
std::string BaseName(const std::string& p) {
    const std::size_t s = p.find_last_of("\\/");
    return (s == std::string::npos) ? p : p.substr(s + 1);
}

// 追加一行到日志（打开→写→关，和 wem 日志同一套做法，截断/占用都不怕）
void AppendLog(const std::string& line) {
    if (g_logPath.empty()) return;
    HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    ::WriteFile(h, line.data(), (DWORD)line.size(), &wrote, nullptr);
    ::CloseHandle(h);
    ++g_stats.logLines;
}

// ---------------------------------------------------------------------------
//  内存特征码扫描：只在主模块的**可执行段**里找 5 字节跳板
// ---------------------------------------------------------------------------
struct SecInfo { std::uint8_t* base; std::size_t size; };

std::vector<SecInfo> ExecSections() {
    std::vector<SecInfo> out;
    HMODULE mod = ::GetModuleHandleW(nullptr);
    if (!mod) return out;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const std::uint8_t*>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const std::size_t sz = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (!sz) continue;
        out.push_back({ reinterpret_cast<std::uint8_t*>(mod) + sec->VirtualAddress, sz });
    }
    return out;
}

// 目标处看起来像函数序言吗？（避免把随机字节当函数 hook）
bool LooksLikeFunction(const std::uint8_t* p) {
    struct Pat { unsigned char a, b, c; bool two; };
    static const Pat pats[] = {
        { 0x48, 0x89, 0x5C, false },   // mov [rsp+x], rbx
        { 0x48, 0x89, 0x4C, false },   // mov [rsp+x], rcx
        { 0x48, 0x83, 0xEC, false },   // sub rsp, imm8
        { 0x48, 0x81, 0xEC, false },   // sub rsp, imm32
        { 0x48, 0x8B, 0xC4, false },   // mov rax, rsp
        { 0x4C, 0x8B, 0xDC, false },   // mov r11, rsp
        { 0x40, 0x53, 0x00, true  },   // push rbx
        { 0x40, 0x55, 0x00, true  },   // push rbp
        { 0x40, 0x56, 0x00, true  },   // push rsi
        { 0x40, 0x57, 0x00, true  },   // push rdi
        { 0x41, 0x54, 0x00, true  },   // push r12
        { 0x41, 0x55, 0x00, true  },   // push r13
        { 0x41, 0x56, 0x00, true  },   // push r14
        { 0x41, 0x57, 0x00, true  },   // push r15
        { 0x55, 0x48, 0x00, true  },   // push rbp; mov ...
    };
    for (const auto& q : pats) {
        if (q.two) { if (p[0] == q.a && p[1] == q.b) return true; }
        else       { if (p[0] == q.a && p[1] == q.b && p[2] == q.c) return true; }
    }
    return false;
}

// 扫内存找 Effect:Ctor 入口（返回 thunk 地址；0 = 没找到）

// ---------------------------------------------------------------------------
//  发现模式（EfxDiscover=1）：找出"武器特效到底从哪个函数被创建"
//
//  背景：挂在 Effect:Ctor（vfx-forge 记录的入口）上时，实测一次游玩只钩到
//  3 种 (表,组,记录)，而 other/raw 都是 0 —— 说明绝大多数特效**不走这个函数**。
//
//  做法（纯观察，不改参数/返回值）：
//   1) 第一次 hook 命中时记下 cPlayerEffect*（Effect:Ctor 的第 1 个参数）；
//   2) 主循环里读它的 5 个 provider 槽（vfx-forge: 0x20/0x28/0x718/0x760/0xB70）；
//   3) 每个 provider 读虚表，把前 8 个槽挂钩（只挂游戏模块内的可执行地址）；
//   4) 每次调用记一行 efxcall slot=N a1= a2=（去重 + 上限），主线程写盘。
//  看日志里哪个 slot 的 a1/a2 像 (组, 记录) → 那就是要挂的真正入口。
// ---------------------------------------------------------------------------
const int kDiscProviders = 5;
const int kDiscSlotsPer = 16;
const int kDiscHooks = kDiscProviders * kDiscSlotsPer;   // 80 个独立 hook 函数

struct DiscSlot {
    std::uintptr_t target = 0;
    int providerIdx = -1;
    int slot = -1;
    void* orig = nullptr;
};
DiscSlot g_disc[kDiscHooks];
std::atomic<std::uintptr_t> g_playerEffect{ 0 };
bool g_discover = true;
bool g_discTried = false;
int  g_discHooked = 0;

// 记录一条发现模式调用（hook 里只入队，写盘在主线程）
void PushDiscover(std::uint32_t idx, std::uint64_t a1, std::uint64_t a2) {
    const std::uint32_t h = g_head.load(std::memory_order_relaxed);
    const std::uint32_t next = (h + 1) % kRingSize;
    if (next == g_tail.load(std::memory_order_acquire)) return;
    g_ring[h].group = 0xFFFFFFFFu;      // 标记：发现模式记录
    g_ring[h].rec = idx;
    g_ring[h].tick = ::GetTickCount64();
    g_ring[h].a1 = a1;
    g_ring[h].a2 = a2;
    g_head.store(next, std::memory_order_release);
}

// 每个虚表槽一个独立 hook（这样才能各自转发到自己的原函数）
template <int I>
void* __fastcall Hook_Disc(void* self, void* a1, void* a2, void* a3) {
    if (g_discover) {
        // 每个槽记最多 6 个**不同的** (a1,a2) 样本（以前只记第一条 → 后面的真调用看不到）
        static std::uint64_t s_keys[6] = {};
        static int s_cnt = 0;
        const std::uint64_t k1 = (std::uint64_t)(std::uintptr_t)a1;
        const std::uint64_t k2 = (std::uint64_t)(std::uintptr_t)a2;
        bool dup = false;
        for (int j = 0; j < s_cnt; ++j) if (s_keys[j] == (k1 ^ (k2 * 0x9E3779B97F4A7C15ull))) { dup = true; break; }
        if (!dup && s_cnt < 6) {
            s_keys[s_cnt++] = k1 ^ (k2 * 0x9E3779B97F4A7C15ull);
            PushDiscover((std::uint32_t)I, k1, k2);
        }
    }
    DiscSlot& d = g_disc[I];
    using Fn = void* (__fastcall*)(void*, void*, void*, void*);
    return d.orig ? reinterpret_cast<Fn>(d.orig)(self, a1, a2, a3) : nullptr;
}

#define WSE_DISC_ENTRY(I) &Hook_Disc<I>,
using DiscHookFn = void* (__fastcall*)(void*, void*, void*, void*);
DiscHookFn const kDiscHookTable[kDiscHooks] = {
    WSE_DISC_ENTRY(0)  WSE_DISC_ENTRY(1)  WSE_DISC_ENTRY(2)  WSE_DISC_ENTRY(3)
    WSE_DISC_ENTRY(4)  WSE_DISC_ENTRY(5)  WSE_DISC_ENTRY(6)  WSE_DISC_ENTRY(7)
    WSE_DISC_ENTRY(8)  WSE_DISC_ENTRY(9)  WSE_DISC_ENTRY(10) WSE_DISC_ENTRY(11)
    WSE_DISC_ENTRY(12) WSE_DISC_ENTRY(13) WSE_DISC_ENTRY(14) WSE_DISC_ENTRY(15)
    WSE_DISC_ENTRY(16) WSE_DISC_ENTRY(17) WSE_DISC_ENTRY(18) WSE_DISC_ENTRY(19)
    WSE_DISC_ENTRY(20) WSE_DISC_ENTRY(21) WSE_DISC_ENTRY(22) WSE_DISC_ENTRY(23)
    WSE_DISC_ENTRY(24) WSE_DISC_ENTRY(25) WSE_DISC_ENTRY(26) WSE_DISC_ENTRY(27)
    WSE_DISC_ENTRY(28) WSE_DISC_ENTRY(29) WSE_DISC_ENTRY(30) WSE_DISC_ENTRY(31)
    WSE_DISC_ENTRY(32) WSE_DISC_ENTRY(33) WSE_DISC_ENTRY(34) WSE_DISC_ENTRY(35)
    WSE_DISC_ENTRY(36) WSE_DISC_ENTRY(37) WSE_DISC_ENTRY(38) WSE_DISC_ENTRY(39)
    WSE_DISC_ENTRY(40) WSE_DISC_ENTRY(41) WSE_DISC_ENTRY(42) WSE_DISC_ENTRY(43)
    WSE_DISC_ENTRY(44) WSE_DISC_ENTRY(45) WSE_DISC_ENTRY(46) WSE_DISC_ENTRY(47)
    WSE_DISC_ENTRY(48) WSE_DISC_ENTRY(49) WSE_DISC_ENTRY(50) WSE_DISC_ENTRY(51)
    WSE_DISC_ENTRY(52) WSE_DISC_ENTRY(53) WSE_DISC_ENTRY(54) WSE_DISC_ENTRY(55)
    WSE_DISC_ENTRY(56) WSE_DISC_ENTRY(57) WSE_DISC_ENTRY(58) WSE_DISC_ENTRY(59)
    WSE_DISC_ENTRY(60) WSE_DISC_ENTRY(61) WSE_DISC_ENTRY(62) WSE_DISC_ENTRY(63)
    WSE_DISC_ENTRY(64) WSE_DISC_ENTRY(65) WSE_DISC_ENTRY(66) WSE_DISC_ENTRY(67)
    WSE_DISC_ENTRY(68) WSE_DISC_ENTRY(69) WSE_DISC_ENTRY(70) WSE_DISC_ENTRY(71)
    WSE_DISC_ENTRY(72) WSE_DISC_ENTRY(73) WSE_DISC_ENTRY(74) WSE_DISC_ENTRY(75)
    WSE_DISC_ENTRY(76) WSE_DISC_ENTRY(77) WSE_DISC_ENTRY(78) WSE_DISC_ENTRY(79)
};
#undef WSE_DISC_ENTRY

// 带保护的内存读（游戏指针可能是垃圾，读错会崩）
bool SafeReadPtr(std::uintptr_t addr, std::uintptr_t& out) {
    __try { out = *reinterpret_cast<std::uintptr_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// 地址是否落在游戏主模块的**可执行**段内（防止把数据当函数挂）
bool InGameCode(std::uintptr_t addr) {
    for (const SecInfo& s : ExecSections()) {
        const std::uintptr_t b = reinterpret_cast<std::uintptr_t>(s.base);
        if (addr >= b && addr < b + s.size) return true;
    }
    return false;
}

// 主线程调用：装好发现模式的钩子（只试一次）
void SetupDiscover() {
    if (!g_discover || g_discTried) return;
    const std::uintptr_t mgr = g_playerEffect.load();
    if (!mgr) return;                     // 还没钩到过 Effect:Ctor，等下次
    g_discTried = true;

    const std::uint32_t kSlots[kDiscProviders] = { 0x20, 0x28, 0x718, 0x760, 0xB70 };
    int idx = 0;
    char note[256];
    std::string detail, slotList;
    for (int p = 0; p < kDiscProviders && idx < kDiscHooks; ++p) {
        std::uintptr_t prov = 0;
        if (!SafeReadPtr(mgr + kSlots[p], prov) || !prov) continue;
        std::uintptr_t vt = 0;
        if (!SafeReadPtr(prov, vt) || !vt) continue;
        { char sb[32]; _snprintf_s(sb, _TRUNCATE, "%s0x%X:%d", slotList.empty() ? "" : ",", kSlots[p], kDiscSlotsPer); slotList += sb; }
        for (int s = 0; s < kDiscSlotsPer && idx < kDiscHooks; ++s, ++idx) {
            std::uintptr_t fn = 0;
            if (!SafeReadPtr(vt + (std::uintptr_t)s * 8, fn) || !fn) continue;
            if (!InGameCode(fn)) continue;                 // 只挂游戏代码段里的
            g_disc[idx].target = fn;
            g_disc[idx].providerIdx = p;
            g_disc[idx].slot = s;
            g_disc[idx].orig = nullptr;
            if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(kDiscHookTable[idx]),
                              reinterpret_cast<LPVOID*>(&g_disc[idx].orig)) == MH_OK && g_disc[idx].orig) {
                if (MH_EnableHook(reinterpret_cast<LPVOID>(fn)) == MH_OK) {
                    ++g_discHooked;
                    char one[48];
                    _snprintf_s(one, _TRUNCATE, "%sP%d.S%d", detail.empty() ? "" : ",", p, s);
                    detail += one;
                }
            }
        }
    }
    _snprintf_s(note, _TRUNCATE,
                "# efx discover: playerEffect=0x%llX hooked=%d slots=[%.*s] detail=[%s]\n",
                (unsigned long long)mgr, g_discHooked, (int)slotList.size(), slotList.c_str(), detail.c_str());
    AppendLog(note);
}

// ---------------------------------------------------------------------------
//  多跳板挂钩：Effect 创建入口是一张**跳板表**，每条都是
//      E9 FB C8 FF FF   (jmp rel32 = -14085 = 0x3705)
//  只是所在地址不同 → 目标不同：
//      +AE0090 → +ADC990   （LuaEngine 的 AddEffect 用的就是这个）
//      +AE0D70 → +ADD670   （vfx-forge 记录的入口）
//  以前只挂了**第一个**匹配（+AE0D70），所以只捕获到少数几种特效；
//  实测一次游玩只有 3 种 (表,组,记录)、other/raw 都是 0 —— 就是漏在其它跳板上。
//
//  现在：扫描所有匹配的跳板（目标必须是像函数的代码），逐个挂钩，
//  每条记录带 via=Tn，日志里能看出"哪类特效走哪个入口"。
// ---------------------------------------------------------------------------
const int kMaxThunks = 32;
using EfxFn = void* (__fastcall*)(void* self, int group, int record);
EfxFn         g_thunkOrig[kMaxThunks] = {};
std::uintptr_t g_thunkAddr[kMaxThunks] = {};
std::uintptr_t g_thunkBody[kMaxThunks] = {};
int           g_thunkCount = 0;

// 往环形缓冲塞一条捕获（hook 里只做这个）
void PushCall(std::uint32_t via, std::uint32_t group, std::uint32_t rec) {
    const std::uint32_t h = g_head.load(std::memory_order_relaxed);
    const std::uint32_t next = (h + 1) % kRingSize;
    if (next == g_tail.load(std::memory_order_acquire)) return;
    g_ring[h].group = group;
    g_ring[h].rec = rec;
    g_ring[h].tick = ::GetTickCount64();
    g_ring[h].via = via;
    g_ring[h].a1 = 0;
    g_ring[h].a2 = 0;
    g_head.store(next, std::memory_order_release);
}

template <int I>
void* __fastcall Hook_EfxThunk(void* self, int group, int record) {
    if (self && g_playerEffect.load(std::memory_order_relaxed) == 0)
        g_playerEffect.store(reinterpret_cast<std::uintptr_t>(self), std::memory_order_release);
    if (g_enabled) ++g_stats.calls;   // ★ 多跳板版忘了这一句 → 统计里 calls 恒为 0（实测抓到的）
    if (g_enabled) PushCall((std::uint32_t)I, (std::uint32_t)group, (std::uint32_t)record);
    return g_thunkOrig[I] ? g_thunkOrig[I](self, group, record) : nullptr;
}

#define WSE_THUNK_ENTRY(I) &Hook_EfxThunk<I>,
using ThunkHookFn = void* (__fastcall*)(void*, int, int);
ThunkHookFn const kThunkHookTable[kMaxThunks] = {
    WSE_THUNK_ENTRY(0)  WSE_THUNK_ENTRY(1)  WSE_THUNK_ENTRY(2)  WSE_THUNK_ENTRY(3)
    WSE_THUNK_ENTRY(4)  WSE_THUNK_ENTRY(5)  WSE_THUNK_ENTRY(6)  WSE_THUNK_ENTRY(7)
    WSE_THUNK_ENTRY(8)  WSE_THUNK_ENTRY(9)  WSE_THUNK_ENTRY(10) WSE_THUNK_ENTRY(11)
    WSE_THUNK_ENTRY(12) WSE_THUNK_ENTRY(13) WSE_THUNK_ENTRY(14) WSE_THUNK_ENTRY(15)
    WSE_THUNK_ENTRY(16) WSE_THUNK_ENTRY(17) WSE_THUNK_ENTRY(18) WSE_THUNK_ENTRY(19)
    WSE_THUNK_ENTRY(20) WSE_THUNK_ENTRY(21) WSE_THUNK_ENTRY(22) WSE_THUNK_ENTRY(23)
    WSE_THUNK_ENTRY(24) WSE_THUNK_ENTRY(25) WSE_THUNK_ENTRY(26) WSE_THUNK_ENTRY(27)
    WSE_THUNK_ENTRY(28) WSE_THUNK_ENTRY(29) WSE_THUNK_ENTRY(30) WSE_THUNK_ENTRY(31)
};
#undef WSE_THUNK_ENTRY

// 扫描所有匹配的跳板（按地址升序），返回 (thunk, body) 对
struct ThunkHit { std::uintptr_t thunk; std::uintptr_t body; };
std::vector<ThunkHit> FindAllThunks(std::string& why) {
    std::vector<ThunkHit> out;
    std::size_t scanned = 0;
    for (const SecInfo& s : ExecSections()) {
        const std::uint8_t* p = s.base;
        const std::uint8_t* end = s.base + s.size;
        scanned += s.size;
        while (p + 5 <= end) {
            const std::uint8_t* hit = static_cast<const std::uint8_t*>(
                ::memchr(p, kSigEffectCtor[0], (std::size_t)(end - p)));
            if (!hit || hit + 5 > end) break;
            if (::memcmp(hit, kSigEffectCtor, 5) == 0) {
                const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(hit + 1);
                const std::uint8_t* target = hit + 5 + rel;
                if (LooksLikeFunction(target)) {
                    ThunkHit h;
                    h.thunk = reinterpret_cast<std::uintptr_t>(hit);
                    h.body = reinterpret_cast<std::uintptr_t>(target);
                    out.push_back(h);
                    if (out.size() >= (std::size_t)kMaxThunks) break;
                }
            }
            p = hit + 1;
        }
        if (out.size() >= (std::size_t)kMaxThunks) break;
    }
    char buf[160];
    _snprintf_s(buf, _TRUNCATE, "扫了 %.1fMB 可执行段，找到 %zu 条跳板",
                (double)scanned / (1024.0 * 1024.0), out.size());
    why = buf;
    return out;
}
// ---------------------------------------------------------------------------
//  hook：只往环形缓冲塞一条（不加锁、不 new、不写盘）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  索引加载
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  索引构建：随包快照 + **磁盘上真实的 .epv3**（nativePC 覆盖优先）
//
//  为什么要合并：快照是参考项目对**原版**表的抓取。玩家改过 epv（换路径、加组/记录）
//  之后，真实表里的 (组,记录) 会多出来 —— 实测 hm_wp11：快照 73 条 vs 磁盘 86 条，
//  差的 13 条会被当成"非武器特效"丢弃（表现就是"有的特效完全捕获不到"）。
// ---------------------------------------------------------------------------
struct DiskTable {
    std::string name;
    std::wstring file;
    std::vector<EpvRec> recs;
};

// 由 g_entries 重建哈希表与表名列表
void RebuildLookup() {
    g_lookup.clear();
    g_tables.clear();
    for (std::size_t i = 0; i < g_entries.size(); ++i) {
        const Entry& en = g_entries[i];
        g_lookup[((std::uint64_t)en.group << 32) | en.rec].push_back((int)i);
    }
    // 表名去重（保持稳定顺序）
    for (const Entry& en : g_entries) {
        bool seen = false;
        for (const std::string& t : g_tables) if (t == en.table) { seen = true; break; }
        if (!seen) g_tables.push_back(en.table);
    }
}

// 扫磁盘上的 hm_wp*.epv3，返回解析成功的表（同名只留一份：优先路径最短/规范的）
std::vector<DiskTable> LoadDiskTables(const std::wstring& gameRoot, std::size_t& filesFound,
                                      std::size_t& parseFailed, std::string& firstErr) {
    std::vector<DiskTable> out;
    const std::vector<std::wstring> files = FindEpvFiles(gameRoot);
    filesFound = files.size();
    for (const std::wstring& f : files) {
        EpvTable t;
        std::string why;
        if (!ParseEpv3File(f, t, why)) {
            ++parseFailed;
            if (firstErr.empty()) firstErr = t.file.empty() ? why : (t.file + ": " + why);
            continue;
        }
        // 同名表：保留路径最短的那个（规范的 wp\<家族>\epv\<名>.epv3 最短）
        bool replaced = false;
        for (DiskTable& d : out) {
            if (d.name == t.name) {
                if (f.size() < d.file.size()) { d.file = f; d.recs = t.recs; }
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            DiskTable d;
            d.name = t.name;
            d.file = f;
            d.recs = t.recs;
            out.push_back(std::move(d));
        }
    }
    return out;
}

// 用磁盘表覆盖同名快照表（整体替换：磁盘上的表就是游戏实际加载的那张）
void ApplyDiskTables(const std::vector<DiskTable>& tabs, std::size_t& tablesReplaced,
                     std::size_t& recsAdded, std::size_t& recsRemoved) {
    for (const DiskTable& d : tabs) {
        const std::size_t before = g_entries.size();
        std::vector<Entry> keep;
        keep.reserve(g_entries.size());
        for (const Entry& en : g_entries) if (en.table != d.name) keep.push_back(en);
        g_entries.swap(keep);
        recsRemoved += before - g_entries.size();
        for (const EpvRec& r : d.recs) {
            Entry en;
            en.table = d.name;
            en.path = r.path;
            en.recId = r.rid;
            en.group = (std::uint32_t)r.group;
            en.rec = (std::uint32_t)r.rec;
            g_entries.push_back(std::move(en));
        }
        recsAdded += d.recs.size();
        ++tablesReplaced;
    }
}

bool LoadIndex(std::string& why);   // 前向声明：BuildIndex 会用它（定义在后面）
// 构建索引 = 随包快照 + 磁盘真实表（覆盖同名）
struct IndexStats {
    std::size_t filesFound = 0, parseFailed = 0, tablesReplaced = 0;
    std::size_t recsAdded = 0, recsRemoved = 0;
    std::string firstErr;
};
bool BuildIndex(const std::wstring& gameRoot, std::string& why, std::string& summary) {
    g_entries.clear();
    g_lookup.clear();
    g_tables.clear();
    std::string bundledWhy;
    const bool bundledOk = LoadIndex(bundledWhy);
    IndexStats st;
    const std::vector<DiskTable> disk = LoadDiskTables(gameRoot, st.filesFound, st.parseFailed, st.firstErr);
    ApplyDiskTables(disk, st.tablesReplaced, st.recsAdded, st.recsRemoved);
    RebuildLookup();
    if (g_entries.empty()) { why = bundledOk ? "索引为空" : bundledWhy; return false; }
    char buf[512];
    _snprintf_s(buf, _TRUNCATE,
                "索引 %zu 表 / %zu 条（快照%s + 磁盘 %zu 个 epv 文件，覆盖 %zu 表：+%zu/-%zu 条%s）",
                g_tables.size(), g_entries.size(), bundledOk ? "OK" : "缺失",
                st.filesFound, st.tablesReplaced, st.recsAdded, st.recsRemoved,
                st.parseFailed ? "，有文件解析失败" : "");
    summary = buf;
    if (st.parseFailed && !st.firstErr.empty()) summary += "（首个失败：" + st.firstErr + "）";
    return true;
}

bool LoadIndex(std::string& why) {
    const std::wstring path = g_dir + std::wstring(kIndexName, kIndexName + std::strlen(kIndexName));
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) { why = "打不开 WseEfxIndex.txt"; return false; }
    std::string text;
    char buf[4096];
    std::size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    std::size_t p0 = 0;
    std::size_t skipped = 0;
    while (p0 < text.size()) {
        std::size_t e = text.find('\n', p0);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p0, e - p0);
        p0 = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.compare(0, 7, "#TABLE|") == 0) {
            const std::size_t b1 = line.find('|', 7);
            if (b1 != std::string::npos) {
                const std::string tn = line.substr(7, b1 - 7);
                // ★ 只认武器特效表：hm_wpXX（含 hm_wp03_01 这类变体）。
                //   用户实测结论："武器的特效全是 hm_wp 开头的"。别的表（怪物/环境/UI）一律不收。
                if (tn.compare(0, 5, "hm_wp") == 0) g_tables.push_back(tn);
                else ++skipped;
            }
            continue;
        }
        // 表名 \t 组 \t 下标 \t id \t 路径
        std::string f5[5];
        int k = 0;
        std::size_t q = 0;
        while (k < 5) {
            std::size_t t = line.find('\t', q);
            if (t == std::string::npos) { f5[k++] = line.substr(q); break; }
            f5[k++] = line.substr(q, t - q);
            q = t + 1;
        }
        if (k < 4 || f5[0].empty()) continue;
        if (f5[0].compare(0, 5, "hm_wp") != 0) { ++skipped; continue; }   // 非武器表：不收录
        Entry en;
        en.table = f5[0];
        en.group = (std::uint32_t)std::atoi(f5[1].c_str());
        en.rec   = (std::uint32_t)std::atoi(f5[2].c_str());
        en.recId = std::atoi(f5[3].c_str());
        en.path  = f5[4];
        g_lookup[((std::uint64_t)en.group << 32) | en.rec].push_back((int)g_entries.size());
        g_entries.push_back(std::move(en));
    }
    if (g_entries.empty()) { why = "WseEfxIndex.txt 里没有 hm_wp* 武器表的条目"; return false; }
    if (skipped) g_skipped = skipped;
    RebuildLookup();
    return true;
}

// 从候选表里挑一张：优先"当前武器类型对应的 hm_wpNN"，否则取名字最短的（基础表）
const Entry* PickEntry(const std::vector<int>& cands) {
    if (cands.empty()) return nullptr;
    const int w = g_weaponFn ? g_weaponFn() : -1;
    const Entry* best = nullptr;
    // ① 用户指定的"这把武器用哪张表"优先（同一 (组,记录) 在多个变体里都有，只能靠这个区分）
    if (w >= 0 && w < 14 && !g_tablePref[w].empty()) {
        for (const int i : cands)
            if (g_entries[i].table == g_tablePref[w]) return &g_entries[i];
    }
    int bestScore = -1;
    for (const int i : cands) {
        const Entry& en = g_entries[i];
        int score = 0;
        // hm_wpNN / hm_wpNN_xx：前缀 "hm_wp" 是 5 个字符，两位数字在下标 5、6。
        // ★ 这里错过两次：先写成 compare(0,6,"hm_wp") + size()>=8，而 "hm_wp03" 只有
        //   7 个字符、数字在 5/6 → 判定永远不成立，退化成"取索引里第一条"，
        //   于是拿太刀（w3）却记成 hm_wp11。实测日志 + 自检一起抓到的。
        if (w >= 0 && en.table.size() >= 7 && en.table.compare(0, 5, "hm_wp") == 0) {
            const char d0 = en.table[5], d1 = en.table[6];
            if (d0 >= '0' && d0 <= '9' && d1 >= '0' && d1 <= '9' &&
                ((d0 - '0') * 10 + (d1 - '0')) == w) score += 100;
        }
        // 同一 (组,记录) 在多个变体表里都有（hm_wp04 / hm_wp04_01）：优先基础表
        if (en.table.find('_', 5) == std::string::npos) score += 50;
        score += (int)(100 - std::min<std::size_t>(100, en.table.size()));   // 越短越优先
        if (score > bestScore) { bestScore = score; best = &en; }
    }
    // 顺便把候选表名记下来（界面显示"这条记录可能属于哪些表"）
    g_lastCand.clear();
    for (std::size_t i = 0; i < cands.size() && i < 6; ++i) {
        const Entry& en = g_entries[cands[i]];
        if (g_lastCand.find(en.table) != std::string::npos) continue;
        if (!g_lastCand.empty()) g_lastCand += ",";
        g_lastCand += en.table;
    }
    return best;
}

} // namespace

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------
bool Init(const std::wstring& moduleDir, std::string& log) {
    g_dir = moduleDir;
    g_logPath = moduleDir + kLogName;

    // 1) 索引
    // 游戏根目录：<game>\nativePC\plugins\ → 上三级（用于扫 nativePC 下真实的 .epv3）
    {
        std::wstring r = moduleDir;
        while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
        // plugins\ 的上两级 = 游戏根（以前写了 3 级 → 扫不到 nativePC → 磁盘 0 个 epv 文件）
        for (int i = 0; i < 2 && !r.empty(); ++i) {
            const std::size_t s = r.find_last_of(L"\\/");
            r = (s == std::wstring::npos) ? std::wstring() : r.substr(0, s);
        }
        if (!r.empty()) g_gameRoot = r + L"\\";
    }

    // 1) 索引：随包快照 + 磁盘上真实的 .epv3（玩家改过表时以后者为准）
    std::string why, idxSummary;
    if (!BuildIndex(g_gameRoot, why, idxSummary)) {
        g_status = "索引加载失败：" + why;
        log += "[efx] " + g_status + "\n";
        AppendLog("# efx 初始化失败: " + g_status + "\n");
        return false;
    }

    // 2) 扫内存找**所有**特效跳板（跳板表里每条对应一类特效入口）
    std::string scanWhy;
    const std::vector<ThunkHit> thunks = FindAllThunks(scanWhy);
    if (thunks.empty()) {
        g_status = "未找到特效函数：" + scanWhy;
        log += "[efx] " + g_status + "\n";
        AppendLog("# efx 初始化失败: " + g_status + "\n");
        return false;
    }

    // 3) 逐个挂钩（每个跳板一个独立 hook，才能各自转发）
    if (MH_Initialize() != MH_OK) {
        // 已经初始化过也算可用（SonarAudio 可能先初始化了）
    }
    std::string hookedList;
    for (std::size_t i = 0; i < thunks.size() && (int)i < kMaxThunks; ++i) {
        const std::uintptr_t t = thunks[i].thunk;
        if (MH_CreateHook(reinterpret_cast<LPVOID>(t), reinterpret_cast<LPVOID>(kThunkHookTable[i]),
                          reinterpret_cast<LPVOID*>(&g_thunkOrig[i])) != MH_OK || !g_thunkOrig[i]) {
            g_thunkOrig[i] = nullptr;
            continue;
        }
        if (MH_EnableHook(reinterpret_cast<LPVOID>(t)) != MH_OK) { g_thunkOrig[i] = nullptr; continue; }
        g_thunkAddr[i] = t;
        g_thunkBody[i] = thunks[i].body;
        ++g_thunkCount;
        char one[48];
        _snprintf_s(one, _TRUNCATE, "%sT%zu@0x%llX", hookedList.empty() ? "" : ",", i,
                    (unsigned long long)(t - reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr))));
        hookedList += one;
    }
    if (!g_thunkCount) {
        g_status = "MH_CreateHook/EnableHook 全部失败";
        log += "[efx] " + g_status + "\n";
        AppendLog("# efx 初始化失败: " + g_status + "\n");
        return false;
    }
    g_hookAddr = g_thunkAddr[0];
    g_hooked = true;

    HMODULE mod = ::GetModuleHandleW(nullptr);
    char st[256];
    _snprintf_s(st, _TRUNCATE,
                "挂钩成功：%d 条跳板 [%s] · %s",
                g_thunkCount, hookedList.c_str(), idxSummary.c_str());
    g_indexSummary = idxSummary;
    g_status = st;
    log += "[efx] " + g_status + "\n";
    {
        char hdr[384];
        _snprintf_s(hdr, _TRUNCATE,
                    "# sonarefx hook=ok thunks=%d [%s] tables=%zu entries=%zu sig=%s\n",
                    g_thunkCount, hookedList.c_str(),
                    g_tables.size(), g_entries.size(), scanWhy.c_str());
        AppendLog(hdr);
        AppendLog("# sonarefx index: " + idxSummary + "\n");   // 界面直接显示索引来源与规模
    }
    return true;
}

void Shutdown() {
    if (g_hooked && g_hookAddr) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_hookAddr));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_hookAddr));
        g_hooked = false;
    }
}

bool Available() { return g_hooked; }
const std::string& Status() { return g_status; }
void SetEnabled(bool on) { g_enabled = on; }
bool Enabled() { return g_enabled; }
void SetLogAll(bool on) { g_logAll = on; }
void SetDiscover(bool on) { g_discover = on; }

// ini: EfxTablePref=3=hm_wp03_25,4=hm_wp04_02  （同一 (组,记录) 可能属于多张变体表，用它指定）
void SetTablePrefs(const std::string& spec) {
    for (int i = 0; i < 14; ++i) g_tablePref[i].clear();
    std::size_t p = 0;
    while (p < spec.size()) {
        std::size_t e = spec.find(',', p);
        if (e == std::string::npos) e = spec.size();
        const std::string one = spec.substr(p, e - p);
        p = e + 1;
        const std::size_t eq = one.find('=');
        if (eq == std::string::npos) continue;
        const int w = std::atoi(one.substr(0, eq).c_str());
        std::string t = one.substr(eq + 1);
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
        while (!t.empty() && (t.back() == ' ' || t.back() == '\r')) t.pop_back();
        if (w >= 0 && w < 14 && !t.empty()) g_tablePref[w] = t;
    }
}
void SetWeaponProvider(int (*fn)()) { g_weaponFn = fn; }
Stats GetStats() { return g_stats; }

bool ReloadIndex(std::string& summary) {
    std::string why;
    if (!BuildIndex(g_gameRoot, why, summary)) { summary = why; return false; }
    g_indexSummary = summary;
    AppendLog("# efx index reload: " + summary + "\n");
    return true;
}

const std::string& IndexSummary() { return g_indexSummary; }

bool ReloadIndexFrom(const std::wstring& gameRoot, std::string& summary) {
    g_gameRoot = gameRoot;
    return ReloadIndex(summary);
}

bool LookupEfx(int group, int rec, int weapon, std::string& table, std::string& path) {
    const auto it = g_lookup.find(((std::uint64_t)(std::uint32_t)group << 32) | (std::uint32_t)rec);
    if (it == g_lookup.end()) return false;
    const int saved = weapon;
    // 临时把"当前武器"换成调用方给的值来挑表（PickEntry 依赖 g_weaponFn）
    static int s_override = -1;
    static bool s_hasOverride = false;
    (void)saved;
    s_override = weapon;
    s_hasOverride = true;
    int (*prevFn)() = g_weaponFn;
    g_weaponFn = nullptr;
    const Entry* en = nullptr;
    {
        // 直接复用挑表逻辑：临时把 weapon 作为"当前武器"
        const int w = s_hasOverride ? s_override : -1;
        int bestScore = -1;
        for (const int i : it->second) {
            const Entry& e2 = g_entries[i];
            int score = 0;
            if (w >= 0 && e2.table.size() >= 7 && e2.table.compare(0, 5, "hm_wp") == 0) {
                const char d0 = e2.table[5], d1 = e2.table[6];
                if (d0 >= '0' && d0 <= '9' && d1 >= '0' && d1 <= '9' &&
                    ((d0 - '0') * 10 + (d1 - '0')) == w) score += 100;
            }
            if (e2.table.find('_', 5) == std::string::npos) score += 50;
            score += (int)(100 - std::min<std::size_t>(100, e2.table.size()));
            if (score > bestScore) { bestScore = score; en = &e2; }
        }
    }
    g_weaponFn = prevFn;
    if (!en) return false;
    table = en->table;
    path = en->path;
    return true;
}

void Flush() {
    SetupDiscover();   // 发现模式：第一次钩到 cPlayerEffect 后，把 provider 虚表槽挂上
    if (!g_hooked || !g_enabled) return;
    std::string batch;
    const std::uint64_t now = ::GetTickCount64();
    while (true) {
        const std::uint32_t t = g_tail.load(std::memory_order_relaxed);
        if (t == g_head.load(std::memory_order_acquire)) break;
        const RawCall rc = g_ring[t];
        g_tail.store((t + 1) % kRingSize, std::memory_order_release);

        if (rc.group == 0xFFFFFFFFu) {   // 发现模式记录：某个 provider 虚表槽被调用了
            char dl[192];
            _snprintf_s(dl, _TRUNCATE, "efxcall p=%u s=%u a1=%llu a2=%llu\n",
                        rc.rec / (std::uint32_t)kDiscSlotsPer, rc.rec % (std::uint32_t)kDiscSlotsPer,
                        (unsigned long long)rc.a1, (unsigned long long)rc.a2);
            batch += dl;
            continue;
        }

        const auto it = g_lookup.find(((std::uint64_t)rc.group << 32) | rc.rec);
        if (it == g_lookup.end()) {
            ++g_stats.other;
            // 诊断：把这个"不在索引里"的原始 (组,记录) 记一次（最多 300 个），
            // 用来区分"hook 没被调用"和"调用了但值对不上"。
            if (g_logAll && g_rawSeen.size() < 300) {
                const std::uint64_t k = ((std::uint64_t)rc.group << 32) | rc.rec;
                if (g_rawSeen.find(k) == g_rawSeen.end()) {
                    g_rawSeen[k] = 1;
                    char rl[96];
                    _snprintf_s(rl, _TRUNCATE, "efxraw group=%u rec=%u weapon=%d\n",
                                rc.group, rc.rec, g_weaponFn ? g_weaponFn() : -1);
                    batch += rl;
                }
            }
            continue;   // 不是武器特效
        }
        const Entry* en = PickEntry(it->second);
        if (!en) { ++g_stats.other; continue; }
        ++g_stats.weapon;

        // 去重：同一 (表,组,记录) 300ms 内只记一条
        const std::string key = en->table + "|" + std::to_string(rc.group) + "|" + std::to_string(rc.rec);
        if (key == g_lastLine && now - g_lastTick < 300) { ++g_stats.dup; continue; }
        g_lastLine = key;
        g_lastTick = now;

        const int w = g_weaponFn ? g_weaponFn() : -1;
        char line[512];
        _snprintf_s(line, _TRUNCATE,
                    "efx seq#%u via=T%u table=%s cand=%s group=%u rec=%u id=%d weapon=%d name=%s\n",
                    ++g_seq, rc.via, en->table.c_str(), g_lastCand.c_str(), rc.group, rc.rec, en->recId, w, en->path.c_str());
        batch += line;
    }
    // 定期把统计写进日志（GUI 显示"钩到几次调用 / 几条武器特效"）
    // ★ 只在计数**有变化**时写：否则站着不动会每 5 秒刷一条一模一样的行（实测刷了几十条）。
    {
        static Stats s_last = Stats{};
        const bool changed = (g_stats.calls != s_last.calls || g_stats.weapon != s_last.weapon ||
                              g_stats.other != s_last.other || g_stats.dup != s_last.dup);
        if (changed && now - g_lastStats >= 5000) {
            g_lastStats = now;
            s_last = g_stats;
            char sl[192];
            _snprintf_s(sl, _TRUNCATE, "# stats calls=%llu weapon=%llu other=%llu dup=%llu raw=%zu\n",
                        g_stats.calls, g_stats.weapon, g_stats.other, g_stats.dup, g_rawSeen.size());
            batch += sl;
            // 顺带补发自检/索引行：日志变大后 GUI 只读尾部，头部那两行会被挤出窗口（实测问题）
            if (!g_status.empty()) {
                char sb[384];
                _snprintf_s(sb, _TRUNCATE, "# sonarefx status: %s\n", g_status.c_str());
                batch += sb;
            }
            if (!g_indexSummary.empty()) {
                char ib[384];
                _snprintf_s(ib, _TRUNCATE, "# sonarefx index: %s\n", g_indexSummary.c_str());
                batch += ib;
            }
        }
    }
    if (!batch.empty()) AppendLog(batch);
}

} // namespace sonarefx
