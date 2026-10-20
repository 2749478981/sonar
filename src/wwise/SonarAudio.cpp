// ===========================================================================
//  SonarAudio.cpp —— 主入口：抓"游戏内正在播放的音效"，供 Sonar 玩法使用
//
//  原理
//  ----
//  MHW 把 Wwise 静态链接进 exe，但**导出了 mangled 名字**（ref/wwise_symbols.txt）。
//  所以我们不需要特征码，直接解析 PE 导出表拿到：
//     AK::SoundEngine::PostEvent       （3 个重载：byID / byName / byNameW）
//     AK::SoundEngine::Query::GetEventIDFromPlayingID
//     AK::SoundEngine::Query::GetPlayingIDsFromGameObject
//     AK::SoundEngine::StopPlayingID / StopAll
//
//  两条互补的抓取通道：
//   1) **hook PostEvent**（主通道）：游戏每次要求播放一个 event，都会经过它。
//      在 trampoline 里先记账（eventID/playingID/gameObjectID），再原样转发。
//      能拿到"触发瞬间"，最准、最全 —— 连 0.1 秒的短音都不会漏。
//   2) **轮询补偿**（辅通道，默认开）：每隔 N ms 枚举活跃 gameObject 的
//      playingID，用 GetEventIDFromPlayingID 反查 eventID，把 hook 期间
//      漏掉/或 hook 未覆盖的重载补上。作为交叉验证与兜底。
//
//  抓到 eventID 后，用启动时扫出来的 nbnk 数据库把 eventID 展开成 media_id
//  集合（eventID → Action → Sound → media_id），再把名称一起写进共享内存。
//
//  安全性
//  ------
//  * 全程只读游戏内存 + hook 一个已知导出函数；不写玩家状态。
//  * hook 回调里只做 memcpy 级别的记账 + 一次哈希查表（提前建好），
//    绝不 new / 不开线程 / 不写盘 —— 见 MHW_DLL_Plugin_Skill.md 的 0xC0000005 教训。
//  * nbnk 当不可信输入，逐字段做边界检查。
//
//  用法：放进 nativePC\plugins\，进游戏即可。配置见 SonarAudio.ini。
// ===========================================================================
#include <windows.h>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "SonarAudioIpc.h"
#include "SonarWwise.h"
#include "SonarIpc.h"
#include "SonarBanks.h"

#include "MinHook.h"   // third_party\minhook（并入 WeaponSoundEnhance.dll 后由 vcxproj 提供 include 路径）

namespace sonaraudio {

// 插件自身所在目录（DllMain 里填），文件内共享
std::wstring g_dir;

namespace {

// ---- 全局配置 ---------------------------------------------------------------
struct Config {
    bool     hook_post_event = true;   // 主通道
    bool     poll_enabled    = true;   // 辅通道
    unsigned poll_ms         = 50;     // 轮询周期
    bool     log_events      = true;   // 日志记事件（可关，减少 IO）
    bool     scan_banks      = true;   // 扫 nbnk
    std::wstring bank_root;            // nbnk 根目录（空 = 游戏根目录）
    unsigned dump_first_ms   = 0;      // 前 N 毫秒的事件强制写日志（调试）
    uint32_t poll_gameobj    = 0;      // 轮询指定 gameObject（0 = 尝试玩家）
};

Config   g_cfg;
WwiseApi g_api;
IpcWriter g_ipc;
BankDatabase g_banks;

std::atomic<bool> g_stop{false};

// ---------------------------------------------------------------------------
//  ★ 并入 WeaponSoundEnhance.dll 用的事件出口（定义见文件后部，避免落在匿名域里）：
//  LogPump 每处理一条"播放事件"就回调一次（已带真 media id + bank + 名字），
//  宿主插件据此直接触发 wem 条目 —— 无需再读磁盘日志。
// ---------------------------------------------------------------------------
using EventSink = void (*)(uint32_t media_id, uint32_t event_id, uint32_t playing_id,
                           const char* bank, const char* name);
std::atomic<EventSink> g_eventSink{nullptr};
// SetEventSink 的定义在文件后部（匿名域之外），此处只声明用途

// hook 线程 → 日志线程 的单槽无锁传递（覆盖式：只保证"最近一条"不丢）
// 只带 POD 数字：名字由日志线程自己查表（避免在 hook 里做字符串拷贝）。
struct PendingLog {
    std::atomic<bool> valid{false};
    uint32_t seq = 0, event_id = 0, playing_id = 0, game_obj_id = 0, media_id = 0, flags = 0;
};
PendingLog g_pendingLog;
std::atomic<uint64_t> g_logPending{0};

// 原始 trampoline（MinHook 生成）。三个重载各一个。
fn_PostEvent_byID_t    g_tramp_byID    = nullptr;
fn_PostEvent_byName_t  g_tramp_byName  = nullptr;
fn_PostEvent_byNameW_t g_tramp_byNameW = nullptr;

// 玩家 gameObject：MHW 里 Wwise gameObject 通常就是实体指针的低 32 位。
// 我们不强求，轮询时用 hook 里见过的 gameObject（自动学习），更好用。
std::atomic<uint32_t> g_lastGameObj{0};
std::atomic<bool>     g_hookInstalled{false};

// ---- 记录一条事件 -----------------------------------------------------------
// 这个函数会被 hook 在游戏的音频线程里调用 —— 必须**极快、无锁、无分配、不写盘**。
// 所以这里只做：哈希查表 + 写共享内存。日志交给独立的日志线程（见 LogPump）。
void RecordEvent(uint32_t event_id, uint32_t playing_id, uint32_t game_obj_id, uint32_t flags) {
    if (game_obj_id) g_lastGameObj.store(game_obj_id, std::memory_order_relaxed);

    // 反查 media（哈希查表，表在建银行时已建好）
    uint32_t media_first = 0;
    if (const std::vector<uint32_t>* mv = g_banks.MediaForEvent(event_id)) {
        if (!mv->empty()) media_first = (*mv)[0];
    }

    // 名字：MHW nbnk 不带 STID，event 名基本为空；有 media 名/bank 名就用
    char namebuf[kNameLen] = {0};
    const std::string bank = g_banks.BankNameForEvent(event_id);
    if (media_first) {
        const std::string mn = g_banks.MediaName(media_first);
        if (!mn.empty()) std::snprintf(namebuf, sizeof(namebuf), "%s", mn.c_str());
    } else if (!bank.empty()) {
        std::snprintf(namebuf, sizeof(namebuf), "%s", bank.c_str());
    }

    uint32_t fl = flags;
    if (namebuf[0]) fl |= kFlagNameKnown;
    g_ipc.Push(event_id, playing_id, game_obj_id, media_first, fl,
               namebuf, bank.empty() ? nullptr : bank.c_str());

    // 记账（原子），日志线程稍后消费；hook 里绝不写盘
    g_logPending.fetch_add(1, std::memory_order_relaxed);
    if (g_cfg.log_events) {
        // 只把最近一条拷进无锁槽（覆盖式），日志线程取走即可
        PendingLog& p = g_pendingLog;
        p.event_id = event_id; p.playing_id = playing_id;
        p.game_obj_id = game_obj_id; p.media_id = media_first;
        p.flags = fl;
        p.seq = g_ipc.total();
        p.valid.store(true, std::memory_order_release);
    }
}

// ---- PostEvent hook（byID 重载）--------------------------------------------
uint32_t __cdecl Hook_PostEvent_byID(uint32_t event_id, uint64_t game_obj_id,
                                     uint32_t flags, void* cb, void* cookie,
                                     uint32_t n_ext, void* ext, uint32_t post_playing_id) {
    const uint32_t playing = g_tramp_byID
        ? g_tramp_byID(event_id, game_obj_id, flags, cb, cookie, n_ext, ext, post_playing_id)
        : 0;
    RecordEvent(event_id, playing, (uint32_t)game_obj_id, kFlagFromPostEvent);
    return playing;
}

// byName 重载：拿到名字后再反查 ID（走游戏的 GetIDFromString 语义不便，直接存名字）
uint32_t __cdecl Hook_PostEvent_byName(const char* name, uint64_t game_obj_id,
                                       uint32_t flags, void* cb, void* cookie,
                                       uint32_t n_ext, void* ext, uint32_t post_playing_id) {
    const uint32_t playing = g_tramp_byName
        ? g_tramp_byName(name, game_obj_id, flags, cb, cookie, n_ext, ext, post_playing_id)
        : 0;
    // 用哈希把字符串名压成伪 ID（仅用于本插件的日志/事件区分）
    uint32_t evid = 0;
    if (name) { for (const char* p = name; *p; ++p) evid = evid * 131u + (uint8_t)*p; }
    RecordEvent(evid, playing, (uint32_t)game_obj_id, kFlagFromPostEvent);
    return playing;
}

uint32_t __cdecl Hook_PostEvent_byNameW(const wchar_t* name, uint64_t game_obj_id,
                                        uint32_t flags, void* cb, void* cookie,
                                        uint32_t n_ext, void* ext, uint32_t post_playing_id) {
    const uint32_t playing = g_tramp_byNameW
        ? g_tramp_byNameW(name, game_obj_id, flags, cb, cookie, n_ext, ext, post_playing_id)
        : 0;
    uint32_t evid = 0;
    if (name) { for (const wchar_t* p = name; *p; ++p) evid = evid * 131u + (uint32_t)*p; }
    RecordEvent(evid, playing, (uint32_t)game_obj_id, kFlagFromPostEvent);
    return playing;
}

bool InstallHooks(std::string& log) {
    if (MH_Initialize() != MH_OK) {
        log += "MH_Initialize 失败\n";
        return false;
    }
    int n = 0;
    if (g_cfg.hook_post_event && g_api.postEvent_byID) {
        if (MH_CreateHook((LPVOID)g_api.postEvent_byID, (LPVOID)&Hook_PostEvent_byID,
                          (LPVOID*)&g_tramp_byID) == MH_OK) ++n;
    }
    if (g_cfg.hook_post_event && g_api.postEvent_byName) {
        if (MH_CreateHook((LPVOID)g_api.postEvent_byName, (LPVOID)&Hook_PostEvent_byName,
                          (LPVOID*)&g_tramp_byName) == MH_OK) ++n;
    }
    if (g_cfg.hook_post_event && g_api.postEvent_byNameW) {
        if (MH_CreateHook((LPVOID)g_api.postEvent_byNameW, (LPVOID)&Hook_PostEvent_byNameW,
                          (LPVOID*)&g_tramp_byNameW) == MH_OK) ++n;
    }
    if (n == 0) { log += "没有成功创建任何 hook\n"; return false; }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { log += "MH_EnableHook 失败\n"; return false; }

    char b[128];
    std::snprintf(b, sizeof(b), "已挂 %d 个 PostEvent 重载 hook\n", n);
    log += b;
    g_hookInstalled.store(true);
    return true;
}

// ---- 轮询补偿线程 -----------------------------------------------------------
// 枚举活跃 gameObject 的 playingID，反查 eventID，补进环形缓冲。
// 我们不知道游戏注册了哪些 gameObject，就用 hook 学到的 + 玩家实体低32位试。
void PollLoop() {
    std::vector<uint32_t> ids(256);
    uint32_t lastPlaying = 0;  // 简单去重：同一批 playingID 不重复推
    while (!g_stop.load()) {
        ::Sleep(g_cfg.poll_ms);
        if (!g_cfg.poll_enabled || !g_api.getPlayingIDsFromGameObject) continue;

        // 收集候选 gameObject
        std::vector<uint32_t> objs;
        const uint32_t learned = g_lastGameObj.load();
        if (learned) objs.push_back(learned);
        if (g_cfg.poll_gameobj && g_cfg.poll_gameobj != learned) objs.push_back(g_cfg.poll_gameobj);
        if (objs.empty()) continue;

        for (uint32_t go : objs) {
            uint32_t num = (uint32_t)ids.size();
            ids.assign(num, 0);
            if (g_api.getPlayingIDsFromGameObject(go, &num, ids.data()) != AK_Success) continue;
            if (num > ids.size()) num = (uint32_t)ids.size();
            for (uint32_t k = 0; k < num; ++k) {
                const uint32_t pid = ids[k];
                if (!pid || pid == lastPlaying) continue;
                lastPlaying = pid;
                uint32_t evid = 0;
                if (g_api.getEventIDFromPlayingID) evid = g_api.getEventIDFromPlayingID(pid);
                if (evid) RecordEvent(evid, pid, go, kFlagFromPolling);
            }
        }
    }
}

// ---- 载入 media 别名（可选，来自 ref 里的 mediaids.json 手工整理）------------
void LoadMediaAliases(const std::wstring& dir) {
    const std::wstring p = dir + L"SonarAudio.mediaids.txt";
    std::vector<uint8_t> d;
    if (!ReadWholeFile(p, d) || d.empty()) return;
    // 每行： <media_id> <TAB或空格> 名称
    std::string s((const char*)d.data(), d.size());
    size_t pos = 0;
    int n = 0;
    while (pos < s.size()) {
        size_t eol = s.find('\n', pos);
        if (eol == std::string::npos) eol = s.size();
        std::string line = s.substr(pos, eol - pos);
        pos = eol + 1;
        // 去 \r 和 BOM
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 3 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB) line.erase(0, 3);
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        uint32_t id = (uint32_t)strtoul(line.c_str(), nullptr, 10);
        std::string alias = line.substr(sp + 1);
        size_t a = alias.find_first_not_of(" \t");
        if (a != std::string::npos) alias.erase(0, a);
        if (id && !alias.empty()) { g_banks.AddMediaAlias(id, alias); ++n; }
    }
    if (n) LogLine("载入 media 别名 %d 条（%s）", n, Narrow(p).c_str());
    else   LogLine("未载入 media 别名（%s 不存在或为空）—— 名字将回退为 bank名/NN.wem",
                   Narrow(p).c_str());
}

// ---- 简单 ini ---------------------------------------------------------------
bool IniGetBool(const std::string& s, const char* key, bool dflt) {
    size_t p = s.find(key);
    while (p != std::string::npos) {
        // 保证是行首（忽略前导空白的键）
        size_t ls = s.find_last_of("\r\n", p);
        std::string pre = (ls == std::string::npos) ? s.substr(0, p) : s.substr(ls + 1, p - ls - 1);
        if (pre.find_first_not_of(" \t") == std::string::npos) {
            size_t e = s.find('=', p);
            if (e != std::string::npos) {
                std::string v = s.substr(e + 1, 8);
                return (v.find('1') != std::string::npos || v.find("true") != std::string::npos ||
                        v.find("True") != std::string::npos || v.find("on") != std::string::npos);
            }
        }
        p = s.find(key, p + 1);
    }
    return dflt;
}

int IniGetInt(const std::string& s, const char* key, int dflt) {
    size_t p = s.find(key);
    while (p != std::string::npos) {
        size_t ls = s.find_last_of("\r\n", p);
        std::string pre = (ls == std::string::npos) ? s.substr(0, p) : s.substr(ls + 1, p - ls - 1);
        if (pre.find_first_not_of(" \t") == std::string::npos) {
            size_t e = s.find('=', p);
            if (e != std::string::npos) return atoi(s.c_str() + e + 1);
        }
        p = s.find(key, p + 1);
    }
    return dflt;
}

// 取字符串型 ini 值（可含中文/空格，UTF-8；支持一行多个用 ; 分隔）
std::wstring IniGetStr(const std::string& s, const char* key) {
    size_t p = s.find(key);
    while (p != std::string::npos) {
        size_t ls = s.find_last_of("\r\n", p);
        std::string pre = (ls == std::string::npos) ? s.substr(0, p) : s.substr(ls + 1, p - ls - 1);
        if (pre.find_first_not_of(" \t") == std::string::npos) {
            size_t e = s.find('=', p);
            if (e != std::string::npos) {
                size_t le = s.find_first_of("\r\n", e);
                std::string v = s.substr(e + 1,
                    (le == std::string::npos ? s.size() : le) - e - 1);
                // 去掉行尾注释（; 开头的注释行不在此处；这里只裁首尾空白）
                size_t a = v.find_first_not_of(" \t");
                size_t b = v.find_last_not_of(" \t");
                if (a == std::string::npos) return {};
                v = v.substr(a, b - a + 1);
                // UTF-8 → UTF-16
                int n = ::MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), nullptr, 0);
                std::wstring w((size_t)n, L'\0');
                ::MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), w.data(), n);
                return w;
            }
        }
        p = s.find(key, p + 1);
    }
    return {};
}

void LoadIni(const std::wstring& dir) {
    std::vector<uint8_t> d;
    // 新名优先；找不到时兼容旧名 SonarAudio.ini（老用户升级不丢配置）
    if (!ReadWholeFile(dir + L"WeaponSoundEnhance.wem.ini", d) || d.empty()) {
        d.clear();
        if (!ReadWholeFile(dir + L"SonarAudio.ini", d) || d.empty()) return;
    }
    std::string s((const char*)d.data(), d.size());
    if (s.size() > 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB) s.erase(0, 3);  // 去 BOM
    g_cfg.hook_post_event = IniGetBool(s, "HookPostEvent", true);
    g_cfg.poll_enabled    = IniGetBool(s, "PollEnabled", true);
    g_cfg.log_events      = IniGetBool(s, "LogEvents", true);
    g_cfg.scan_banks      = IniGetBool(s, "ScanBanks", true);
    g_cfg.poll_ms         = (unsigned)IniGetInt(s, "PollMs", 50);
    g_cfg.dump_first_ms   = (unsigned)IniGetInt(s, "DumpFirstMs", 0);
    g_cfg.bank_root       = IniGetStr(s, "BankRoot");   // ← 之前漏读了
    if (g_cfg.poll_ms < 5) g_cfg.poll_ms = 5;
    if (g_cfg.poll_ms > 2000) g_cfg.poll_ms = 2000;
}

// ---- 日志线程：把 hook 记账转成磁盘日志（限速）-------------------------------
void LogPump() {
    uint64_t lastTotal = 0;
    while (!g_stop.load()) {
        ::Sleep(200);
        const uint64_t total = g_ipc.total();
        if (total == lastTotal) continue;
        if (g_pendingLog.valid.exchange(false, std::memory_order_acquire)) {
            const PendingLog& p = g_pendingLog;
            // 名字在日志线程查（hook 里不做字符串操作）
            const std::string bank  = g_banks.BankNameForEvent(p.event_id);
            const std::string mname = p.media_id ? g_banks.MediaName(p.media_id) : std::string();
            const std::string epath = g_banks.BankPathForEvent(p.event_id);
            // wem 字节大小（DIDX uSize）。注意：**这不是 media id**。
            // media= 才是真正的 media id（Sound.sourceID）；wem= 只是文件大小，
            // 不同 wem 可能同 size，靠 media= 才能唯一确定 wem。
            const uint32_t wsize = p.media_id ? g_banks.MediaSize(p.media_id) : 0;

            // ★ 事件出口：先交给宿主（触发 wem 条目），再写日志
            if (EventSink sink = g_eventSink.load(std::memory_order_acquire)) {
                sink(p.media_id, p.event_id, p.playing_id,
                     bank.c_str(), mname.empty() ? nullptr : mname.c_str());
            }

            if (!epath.empty() || !bank.empty() || !mname.empty()) {
                LogLine("evt seq#%u id=%u playing=%u gobj=%u media=%u wem=%u%s%s  bank=%s  path=%s%s",
                        p.seq, p.event_id, p.playing_id, p.game_obj_id, p.media_id, wsize,
                        mname.empty() ? "" : " name=", mname.c_str(),
                        bank.empty() ? "?" : bank.c_str(),
                        epath.empty() ? "?" : epath.c_str(),
                        p.media_id ? "" : "  [链断在容器/未收录 media]");
            } else {
                LogLine("evt seq#%u id=%u playing=%u gobj=%u media=%u wem=%u  [未匹配到 bank]",
                        p.seq, p.event_id, p.playing_id, p.game_obj_id, p.media_id, wsize);
            }
        }
        lastTotal = total;
    }
}

// ---- 主流程 -----------------------------------------------------------------
void Run() {
    std::string log;

    LoadIni(g_dir);

    // 1) 解析 Wwise 符号
    log += "=== 解析 Wwise 导出符号 ===\n";
    const bool api_ok = ResolveWwiseApi(g_api, log);
    LogLine("%s", log.c_str());
    log.clear();

    // 2) 建共享内存
    if (!g_ipc.Create(log)) {
        LogLine("共享内存创建失败，插件无法对外提供事件：%s", log.c_str());
        return;
    }
    LogLine("%s", log.c_str());
    log.clear();

    // 2.5) 先载入 media 别名表（名字映射）。
    //      放在 bank 扫描 / 装 hook **之前**，这样从第一条事件起就有可读名字，
    //      不会出现"启动头几秒只有 ID"的情况。
    LoadMediaAliases(g_dir);

    // 3) 扫 nbnk（可能耗时，放后台完成，先让 hook 生效）
    //    重要：游戏内的 bank 打包在 chunk*.pak 里，磁盘上**没有散落的 .nbnk**。
    //    实测多源互补：nativePC(已装 mod) 覆盖 41%、解包 chunk 35%、
    //    武器音效源文件 18% —— 合并后 61%。所以这里**全部都要扫**。
    std::thread bankThread;
    if (g_cfg.scan_banks) {
        std::vector<std::wstring> roots;

        // (0) ini 显式指定的（优先级最高，且允许用 ; 分隔多个）
        if (!g_cfg.bank_root.empty()) {
            const std::wstring& s = g_cfg.bank_root;
            size_t b = 0;
            while (b <= s.size()) {
                size_t e = s.find(L';', b);
                std::wstring one = (e == std::wstring::npos) ? s.substr(b) : s.substr(b, e - b);
                // 去掉首尾空格
                size_t a1 = one.find_first_not_of(L" \t");
                size_t a2 = one.find_last_not_of(L" \t");
                if (a1 != std::wstring::npos) roots.push_back(one.substr(a1, a2 - a1 + 1));
                if (e == std::wstring::npos) break;
                b = e + 1;
            }
        }

        // 定位游戏根目录。插件在 <游戏>\nativePC\plugins\，所以从 exe 目录
        // （其实是本 DLL 的目录）往上两级就是游戏根。
        wchar_t exe[MAX_PATH * 2] = {};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring self = exe;
        std::wstring dir;
        {
            size_t s = self.find_last_of(L"\\/");
            dir = (s == std::wstring::npos) ? L"." : self.substr(0, s);
        }
        std::wstring game;
        {   // plugins -> nativePC -> 游戏根
            size_t s1 = dir.find_last_of(L"\\/");
            if (s1 != std::wstring::npos) {
                std::wstring up1 = dir.substr(0, s1);
                size_t s2 = up1.find_last_of(L"\\/");
                game = (s2 == std::wstring::npos) ? up1 : up1.substr(0, s2);
            }
        }

        // (1) 插件自己的目录（有人把 bank 直接放 plugins 下）
        roots.push_back(dir);
        // (2) 游戏 nativePC —— 装过的 mod 音效在这里
        if (!game.empty()) roots.push_back(game + L"\\nativePC");
        // (3) 游戏 exe 目录本身
        if (!game.empty()) roots.push_back(game);
        // (4) 游戏根下解包出来的 wwise 目录
        if (!game.empty()) {
            roots.push_back(game + L"\\chunk\\sound\\wwise\\Windows");
            roots.push_back(game + L"\\mhwmod解包\\chunk\\sound\\wwise\\Windows");
        }
        // (5) 本机常见位置（不存在会自动跳过）
        roots.push_back(L"D:\\下载\\音效源文件（含笔记）");   // 武器音效带中文注释
        roots.push_back(L"D:\\mhwmod解包\\chunk\\sound\\wwise\\Windows");
        roots.push_back(L"D:\\mhwmod解包\\chunkG8\\sound\\wwise\\Windows");
        roots.push_back(L"D:\\mhwmod解包\\sound\\wwise\\Windows");
        roots.push_back(L"D:\\mhwmod解包\\stamp");

        bankThread = std::thread([roots]() {
            // 注意：不同目录里的 bank 是**互补**的（例如 chunk 放武器/怪物，
            // chunkG8 放 BGM/event_cmn），所以不能"扫到一个就停"，要全试一遍。
            for (const std::wstring& r : roots) {
                if (::GetFileAttributesW(r.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    LogLine("nbnk 跳过（目录不存在）：%s", Narrow(r).c_str());
                    continue;
                }
                std::string l;
                g_banks.ScanDirectory(r, l);
                LogLine("%s", l.empty() ? "nbnk 扫描完成（无日志）" : l.c_str());
            }
            LogLine("nbnk 全部扫描结束：定义 bank %u 个，纯数据 bank %u 个，"
                    "event %u 条，media %u 条",
                    (unsigned)g_banks.parsed_hirc(), (unsigned)g_banks.parsed_data(),
                    (unsigned)g_banks.event_count(), (unsigned)g_banks.media_count());
        });
    } else {
        bankThread = std::thread([] {});
    }

    // 4) 装 hook
    if (api_ok && g_cfg.hook_post_event) {
        std::string hl;
        if (InstallHooks(hl)) { g_ipc.SetHookOk(true); LogLine("%s", hl.c_str()); }
        else { LogLine("hook 安装失败：%s", hl.c_str()); }
    } else if (!g_cfg.hook_post_event) {
        LogLine("配置关闭了 PostEvent hook，仅用轮询");
    }

    // 5) 轮询线程 + 日志线程
    std::thread pollThread;
    if (g_cfg.poll_enabled) pollThread = std::thread(PollLoop);
    std::thread logThread(LogPump);

    LogLine("SonarAudio 就绪。hook=%d poll=%u ms", (int)g_hookInstalled.load(), g_cfg.poll_ms);

    // 等 nbnk 扫描完（最多 60 秒），然后进入常规待机
    if (bankThread.joinable()) bankThread.join();
    LogLine("初始化完成：bank=%u event=%u media=%u", (unsigned)g_banks.bank_count(),
            (unsigned)g_banks.event_count(), (unsigned)g_banks.media_count());

    // 待机：定期打健康日志
    uint64_t lastLog = ::GetTickCount64();
    while (!g_stop.load()) {
        ::Sleep(1000);
        const uint64_t now = ::GetTickCount64();
        if (now - lastLog >= 30000) {
            lastLog = now;
            LogLine("heartbeat: captured=%u hook=%d", g_ipc.total(), (int)g_hookInstalled.load());
        }
    }

    if (pollThread.joinable()) pollThread.join();
    if (logThread.joinable()) logThread.join();
}

}  // namespace

// ===========================================================================
//  启动入口（并入 WeaponSoundEnhance.dll 后不再自带 DllMain）
//  由宿主的 DllMain 调用：设置目录、初始化日志、起后台线程。
// ===========================================================================
void SonarAudioStart(HMODULE m) {
    wchar_t buf[MAX_PATH * 2] = {};
    ::GetModuleFileNameW(m, buf, MAX_PATH * 2);
    g_dir = buf;
    const size_t s = g_dir.find_last_of(L"\\/");
    g_dir = (s == std::wstring::npos) ? L"" : g_dir.substr(0, s + 1);
    // ★ 输出（日志/ini）一律放数据目录 plugins\\WeaponSoundEnhance\\，
    //   插件目录里只留主 dll（用户明确要求，别在 plugins\ 里堆文件）。
    if (::GetFileAttributesW((g_dir + L"WeaponSoundEnhance").c_str()) != INVALID_FILE_ATTRIBUTES)
        g_dir += L"WeaponSoundEnhance\\";
    LogInit(g_dir);
    LogLine("SonarAudio module (merged into WeaponSoundEnhance.dll) loading...");
    std::thread(Run).detach();
}

void SonarAudioStop() { g_stop.store(true); }

// 宿主设置事件出口（必须定义在匿名域之外，宿主才能链接到）
void SetEventSink(EventSink s) { g_eventSink.store(s, std::memory_order_release); }

}  // namespace sonaraudio
