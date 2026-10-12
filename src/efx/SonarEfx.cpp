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
struct RawCall { std::uint32_t group; std::uint32_t rec; std::uint64_t tick; };
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
std::wstring  g_logPath;
bool          g_hooked = false;
std::uintptr_t g_hookAddr = 0;
std::string   g_status = "未初始化";
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
std::uintptr_t FindEffectCtor(std::string& why) {
    const std::vector<SecInfo> secs = ExecSections();
    if (secs.empty()) { why = "拿不到模块可执行段"; return 0; }
    std::size_t scanned = 0;
    for (const auto& s : secs) {
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
                    why = "命中特征码且跳转目标像函数";
                    return reinterpret_cast<std::uintptr_t>(hit);
                }
            }
            p = hit + 1;
        }
    }
    char buf[128];
    _snprintf_s(buf, _TRUNCATE, "扫了 %.1fMB 可执行段，没找到特征码（游戏版本变了？）",
                (double)scanned / (1024.0 * 1024.0));
    why = buf;
    return 0;
}

// ---------------------------------------------------------------------------
//  hook：只往环形缓冲塞一条（不加锁、不 new、不写盘）
// ---------------------------------------------------------------------------
void* __fastcall Hook_EffectCtor(void* self, int group, int record) {
    if (g_enabled) {
        ++g_stats.calls;
        const std::uint32_t h = g_head.load(std::memory_order_relaxed);
        const std::uint32_t next = (h + 1) % kRingSize;
        if (next != g_tail.load(std::memory_order_acquire)) {
            g_ring[h].group = (std::uint32_t)group;
            g_ring[h].rec   = (std::uint32_t)record;
            g_ring[h].tick  = ::GetTickCount64();
            g_head.store(next, std::memory_order_release);
        }
    }
    return g_orig ? g_orig(self, group, record) : nullptr;
}

// ---------------------------------------------------------------------------
//  索引加载
// ---------------------------------------------------------------------------
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
    return true;
}

// 从候选表里挑一张：优先"当前武器类型对应的 hm_wpNN"，否则取名字最短的（基础表）
const Entry* PickEntry(const std::vector<int>& cands) {
    if (cands.empty()) return nullptr;
    const int w = g_weaponFn ? g_weaponFn() : -1;
    const Entry* best = nullptr;
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
    std::string why;
    if (!LoadIndex(why)) {
        g_status = "索引加载失败：" + why;
        log += "[efx] " + g_status + "\n";
        AppendLog("# efx 初始化失败: " + g_status + "\n");
        return false;
    }

    // 2) 扫内存找函数
    std::string scanWhy;
    const std::uintptr_t addr = FindEffectCtor(scanWhy);
    if (!addr) {
        g_status = "未找到特效函数：" + scanWhy;
        log += "[efx] " + g_status + "\n";
        AppendLog("# efx 初始化失败: " + g_status + "\n");
        return false;
    }
    g_hookAddr = addr;

    // 3) hook
    if (MH_Initialize() != MH_OK) {
        // 已经初始化过也算可用（SonarAudio 可能先初始化了）
    }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(addr), reinterpret_cast<LPVOID>(&Hook_EffectCtor),
                      reinterpret_cast<LPVOID*>(&g_orig)) != MH_OK || !g_orig) {
        g_status = "MH_CreateHook 失败";
        log += "[efx] " + g_status + "\n";
        AppendLog("# efx 初始化失败: " + g_status + "\n");
        return false;
    }
    if (MH_EnableHook(reinterpret_cast<LPVOID>(addr)) != MH_OK) {
        g_status = "MH_EnableHook 失败";
        log += "[efx] " + g_status + "\n";
        AppendLog("# efx 初始化失败: " + g_status + "\n");
        return false;
    }
    g_hooked = true;

    HMODULE mod = ::GetModuleHandleW(nullptr);
    char st[256];
    _snprintf_s(st, _TRUNCATE,
                "挂钩成功：RVA 0x%llX（模块基址 0x%llX）· 索引 %zu 张表 / %zu 条记录 · %s",
                (unsigned long long)(addr - reinterpret_cast<std::uintptr_t>(mod)),
                (unsigned long long)reinterpret_cast<std::uintptr_t>(mod),
                g_tables.size(), g_entries.size(), scanWhy.c_str());
    g_status = st;
    log += "[efx] " + g_status + "\n";
    {
        char hdr[384];
        _snprintf_s(hdr, _TRUNCATE,
                    "# sonarefx hook=ok rva=0x%llX tables=%zu entries=%zu sig=%s\n",
                    (unsigned long long)(addr - reinterpret_cast<std::uintptr_t>(mod)),
                    g_tables.size(), g_entries.size(), scanWhy.c_str());
        AppendLog(hdr);
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
void SetWeaponProvider(int (*fn)()) { g_weaponFn = fn; }
Stats GetStats() { return g_stats; }

void Flush() {
    if (!g_hooked || !g_enabled) return;
    std::string batch;
    const std::uint64_t now = ::GetTickCount64();
    while (true) {
        const std::uint32_t t = g_tail.load(std::memory_order_relaxed);
        if (t == g_head.load(std::memory_order_acquire)) break;
        const RawCall rc = g_ring[t];
        g_tail.store((t + 1) % kRingSize, std::memory_order_release);

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
                    "efx seq#%u table=%s group=%u rec=%u id=%d weapon=%d name=%s\n",
                    ++g_seq, en->table.c_str(), rc.group, rc.rec, en->recId, w, en->path.c_str());
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
        }
    }
    if (!batch.empty()) AppendLog(batch);
}

} // namespace sonarefx
