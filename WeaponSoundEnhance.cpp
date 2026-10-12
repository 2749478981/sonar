// ============================================================================
//  WeaponSoundEnhance.cpp  (v2)
//  Monster Hunter: World / Iceborne (15.23.00) weapon-sound plugin.
//
//  Standalone: no game audio engine calls, no code hooks. Player actions are
//  detected by a background polling thread; wav playback uses winmm waveOut
//  (one handle per voice -> true multi-sound overlap).
//
//  Files / paths:
//    <plugins>\WeaponSoundEnhance.dll                                  <- DLL 放这里
//    <plugins>\WeaponSoundEnhance\WeaponSoundEnhance.ini               <- 数据目录(优先)
//    <plugins>\WeaponSoundEnhance\WeaponSoundEnhance.ini.template      <- 随包模板
//    <plugins>\WeaponSoundEnhance\sounds\*.wav                         <- 音效
//    <plugins>\WeaponSoundEnhance\WeaponSoundEnhance.log               <- 日志
//    旧布局(<plugins>\WeaponSoundEnhance.ini 与 DLL 同目录)仍然兼容。
//    用户 ini 缺失时自动由 *.ini.template 生成 —— 更新只覆盖模板，不动用户配置。
//
//  Memory layout (15.23.00, PlayerRoot overridable in ini):
//    manager = *(PlayerRoot)                       [PlayerRoot = 0x1450139A0]
//    entity  = *(manager + 0x50)
//    fsm     = *(*(entity + 0x468) + 0xE9C4)
//    weapon  = *(*(*(entity + 0xC0) + 0x8) + 0x78) ; +0x2E8 = type, +0x2EC = id
//    LS gauge = *(*(entity + GaugePtrOff) + GaugeValOff)   (default 0x76B0/0x2370)
//
//  Config: one [Attack...] section per trigger entry. Legacy keys
//  (WeaponType/ActionLMT/FSMId/Sound/SoundDelay/SoundVol) behave exactly as v1.
//  v2 additions:
//    LMT=...            trigger on any of several LMTs (comma/space list);
//                       空 / -1 / any / * / 不限 = 不限(该 FSMId 的所有动作都触发)
//    Sound:<tag>=...    per-LS-gauge sound set (tag 0..3 or none/white/yellow/red);
//                       missing tag falls back to the default Sound= set
//    Group=...          logical-action id: shared by the trigger entries of one
//                       attack, the group fires once per action occurrence and
//                       freezes the gauge at the first trigger instant
//    path|delay|vol|F   per-sound spec; trailing F marks the sound "fixed"
//                       (always plays; same file is 400ms-cooldowned to avoid
//                       stacking); non-fixed sounds still pick one at random
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "SonarEfx.h"          // 特效捕获（实验性）：hook Effect:Ctor
#include <objbase.h>
#include <mmsystem.h>

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>
#include <map>
#include <unordered_map>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include <utility>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

// 音效解码（wav/mp3/ogg/flac -> 16-bit PCM）：实现宏只在本 TU 展开一次
#define WSE_AUDIO_IMPLEMENTATION
#include "third_party/wse_audio.h"

// ===========================================================================
//  Memory-safe read helpers (every access guarded)
// ===========================================================================
namespace mem {

inline bool IsReadable(std::uintptr_t a, std::size_t n)
{
    if (a == 0 || n == 0) return false;
    for (std::size_t i = 0; i < n; i += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi{};
        std::uintptr_t probe = a + i;
        if (::VirtualQuery((LPCVOID)probe, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect == PAGE_NOACCESS || mbi.Protect == PAGE_GUARD) return false;
        const DWORD rw = mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
        if (rw == 0) return false;
        if (i + 0x1000 >= n) break;
        if (probe + 0x1000 < probe) return false;
    }
    return true;
}

inline bool IsWritable(std::uintptr_t a, std::size_t n)
{
    if (a == 0 || n == 0) return false;
    for (std::size_t i = 0; i < n; i += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi{};
        std::uintptr_t probe = a + i;
        if (::VirtualQuery((LPCVOID)probe, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect == PAGE_NOACCESS || mbi.Protect == PAGE_GUARD) return false;
        const DWORD rw = mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
        if (rw == 0) return false;
        if (i + 0x1000 >= n) break;
        if (probe + 0x1000 < probe) return false;
    }
    return true;
}

template <typename T>
inline bool ReadVal(std::uintptr_t a, T& out)
{
    if (!IsReadable(a, sizeof(T))) return false;
    std::memcpy(&out, reinterpret_cast<const void*>(a), sizeof(T));
    return true;
}

// addr = base ; for each offset: addr = *(addr + off). No deref of base itself.
inline std::uintptr_t Walk(std::uintptr_t base, const std::uint32_t* offs, int count)
{
    if (base == 0) return 0;
    std::uintptr_t cur = base;
    for (int i = 0; i < count; ++i) {
        if (cur == 0) return 0;
        std::uintptr_t next = 0;
        if (!ReadVal(cur + offs[i], next)) return 0;
        cur = next;
    }
    return cur;
}

inline std::int32_t ReadI32(std::uintptr_t a, std::int32_t dflt)
{
    std::int32_t v = dflt;
    if (IsReadable(a, 4)) std::memcpy(&v, reinterpret_cast<const void*>(a), 4);
    return v;
}

inline std::uintptr_t ReadPtr(std::uintptr_t a)
{
    std::uintptr_t v = 0;
    if (IsReadable(a, sizeof(v))) std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(v));
    return v;
}

inline float ReadF32(std::uintptr_t a, float dflt)
{
    float v = dflt;
    if (IsReadable(a, 4)) std::memcpy(&v, reinterpret_cast<const void*>(a), 4);
    return v;
}

} // namespace mem

// ===========================================================================
//  Text conversion (UTF-8 <-> wide)
// ===========================================================================
namespace strconv {

inline std::wstring ToWide(const std::string& s)
{
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n - 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

inline std::string ToUtf8(const std::wstring& w)
{
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

} // namespace strconv

// ===========================================================================
//  Player live state (refreshed by the polling thread)
// ===========================================================================
namespace player {

std::uintptr_t gRoot = 0x1450139A0ULL;   // ini override-able
std::int32_t   gLmt = -1;
std::int32_t   gFsm = -1;
std::int32_t   gWeapon = -1;
std::int32_t   gWeaponId = -1;
std::int32_t   gGauge = -1;              // long-sword spirit gauge level 0..3, -1 unknown
std::uintptr_t gEntity = 0;              // 玩家实体指针（Refresh 时更新；layer 探针用）

// 场景 / 任务（详见 MHW_Memory_Map_Skill.md §3）
//   地图 Id : *(int32*)(*(entity+0x7D20) + 0xB88)   ← 变化 = 换场景（实体指针切场景不变）
//   任务状态: *(int32*)(*(0x14500ED30) + 0x54)      ← ==2 表示任务进行中（集会所里对象非 0，看状态）
std::int32_t   gMapId = -1;
std::int32_t   gQuestState = -1;
std::uint32_t  gMapPtrOff = 0x7D20;      // entity -> 地图数据指针
std::uint32_t  gMapIdOff   = 0xB88;      // 地图数据 -> 地图 Id
std::uint32_t  gQuestStateOff = 0x54;    // 任务对象 -> 任务状态

std::uint32_t gGaugePtrOff = 0x76B0;     // LS spirit object: *(entity + off)
std::uint32_t gGaugeValOff = 0x2370;     // gauge level value: *(obj + off)

// FSM 状态是 (target, id) 二元组寻址的：不同 target 层里的 id 会重号
//（实测 fsmID 102 在通用层是别的动作、在太刀层是大居）。
// 因此条目可选配 FSMTarget= 一起匹配；不写(-1)时只比 id（与旧行为一致），
// 想避免跨层重号误触发就填上 target。target 也可在条件表达式里用 fsmTarget 读。
std::int32_t   gFsmTarget = -1;
std::uint32_t  gFsmTargetOff = 0x6274;   // FSM target 值偏移（ini FsmTargetOff 可覆盖）

// 大剑蓄力等级：和太刀刃级在**同一个对象**上，只是值偏移不同
std::int32_t   gCharge = -1;
std::uint32_t  gChargeValOff = 0x2358;   // 别用 0x2370：大剑上它只从 2 起跳，分不出 1 蓄和没蓄

// ---------------------------------------------------------------------------
//  场景/装备稳定时间戳 —— 用来压制"进图/换区/切装备"时的 wem 误触发。
//
//  背景：wem 是全局音频事件，游戏进集会/换区/换装备时会**预先 PostEvent**
//  一堆武器音效（把资源读进内存），并不是真的在播。表现为：一进集会就听到
//  装瓶音效，但其实没做任何动作。
//
//  判定：每当玩家实体出现、或武器类型发生变化（= 加载/切装完成），就把
//  gStableSinceMs 重置为当前时刻。此后 kWemSettleMs 毫秒内的 wem 一律忽略。
//  （真正做动作至少要等玩家落地/出手，远大于这个窗口。）
// ---------------------------------------------------------------------------
std::uint64_t  gStableSinceMs = 0;    // 上次"实体出现或武器变化"的时刻
std::int32_t   gLastWeaponStable = -2; // 用于检测武器变化（-2 = 未初始化）

// 任务累计伤害。实测**只记你自己打的**，不含队友，所以拿来判命中在联机下也是干净的。
// （对照：怪物血量是全队共享的，不能用来判"我这一下打中没有"。）
std::int32_t   gQuestDmg     = -1;
std::uintptr_t gQuestRoot    = 0x14500ED30ULL;
std::uint32_t  gQuestDmgOff  = 0x17088;

// ---------------------------------------------------------------------------
//  命中信号（零 hook）：直接盯**怪物血量**有没有掉。
//
//  为什么不能只靠任务累计伤害：
//    QuestDmg 在部分场景/版本里读不到（对象没建、换区重置、练习场），
//    一旦读成 -1，条件里的 dmg>0 就永远不成立 —— 玩家的判定模式就"没生效"。
//    血量信号更原始，只要有怪在场就有值。
//
//  为什么不担心队友：dmg 条件只看"相对开窗基准掉了多少"。多人时队友的伤害
//  确实会让血量掉 —— 但那是**开窗这几十~几百毫秒内**、且判定点通常紧贴自己
//  出伤时刻的信号，作为"我这一下打中没有"的近似比 QuestDmg 读不到要好得多。
//  两个信号取"任一有效"：QuestDmg 能读就优先用它（最干净），读不到才用血量。
// ---------------------------------------------------------------------------
float          gMonHp      = -1.0f;   // 当前血量（-1 = 未知）
float          gMonHpMax   = -1.0f;   // 血量上限（用来判掉练习场木桩）
std::uintptr_t gMonPtr     = 0;       // 被盯的怪物实体
float          gMonHpLimit = 1000000.0f;  // 上限保护：读到离谱值说明链子错了
std::uint32_t  gMonHpPtrOff = 0x7670; // *(monster + off) = 生命对象
std::uint32_t  gMonHpMaxOff = 0x60;   // 上限
std::uint32_t  gMonHpCurOff = 0x64;   // 当前值
std::uint32_t  gHitMonOff   = 0x12958;// *(entity + off) = 武器最近命中的怪物

void Refresh()
{
    gLmt = -1; gFsm = -1; gWeapon = -1; gWeaponId = -1; gGauge = -1;
    gFsmTarget = -1; gCharge = -1; gQuestDmg = -1;
    gMapId = -1; gQuestState = -1;

    {
        std::uintptr_t quest = 0;
        if (mem::ReadVal(gQuestRoot, quest) && quest)
            gQuestDmg = mem::ReadI32(quest + gQuestDmgOff, -1);
    }

    std::uintptr_t manager = 0;
    if (!mem::ReadVal(gRoot, manager) || manager == 0) return;

    const std::uint32_t c0[1] = {0x50};
    const std::uintptr_t entity = mem::Walk(manager, c0, 1);
    if (!entity) return;

    gEntity = entity;   // 暴露给 layer 探针（定位装瓶/平射所在的 layer 字段）
    gFsm       = mem::ReadI32(entity + 0x6278, -1);
    // 场景 / 任务（MHW_Memory_Map_Skill.md §3）
    std::uintptr_t mapPtr = 0;
    gMapId = -1;
    if (mem::ReadVal(entity + gMapPtrOff, mapPtr) && mapPtr)
        gMapId = mem::ReadI32(mapPtr + gMapIdOff, -1);
    std::uintptr_t questObj = 0;
    gQuestState = -1;
    if (mem::ReadVal(gQuestRoot, questObj) && questObj)
        gQuestState = mem::ReadI32(questObj + gQuestStateOff, -1);
    gFsmTarget = mem::ReadI32(entity + gFsmTargetOff, -1);

    std::uintptr_t act = 0;
    if (mem::ReadVal(entity + 0x468, act) && act)
        gLmt = mem::ReadI32(act + 0xE9C4, -1);

    static const std::uint32_t wd[3] = {0xC0, 0x8, 0x78};
    const std::uintptr_t data = mem::Walk(entity, wd, 3);
    if (data) {
        gWeapon   = mem::ReadI32(data + 0x2E8, -1);
        gWeaponId = mem::ReadI32(data + 0x2EC, -1);
    }

    // LS gauge level is only meaningful (and only read) for the long sword.
    if (gWeapon == 3) {
        std::uintptr_t sp = 0;
        if (mem::ReadVal(entity + gGaugePtrOff, sp) && sp) {
            const int v = mem::ReadI32(sp + gGaugeValOff, -1);
            if (v >= 0 && v <= 3) gGauge = v;
        }
    }

    // 大剑蓄力等级，复用同一个对象指针
    if (gWeapon == 0) {
        std::uintptr_t sp = 0;
        if (mem::ReadVal(entity + gGaugePtrOff, sp) && sp) {
            const int v = mem::ReadI32(sp + gChargeValOff, -1);
            if (v >= 0 && v <= 3) gCharge = v;
        }
    }

    // ---- 被盯怪物的血量：命中判定的备用信号 ----
    // 0x12958 是"武器最近命中的怪"，命中过就一直保留（粘滞），所以它只用来
    // **锁定目标**，掉血与否仍靠逐帧比较血量。目标为空/血量对象读不出时，
    // 保留上一帧的值不动（宁可用稍旧的血量，也别把信号打成"不可用"）。
    {
        const std::uintptr_t mon = mem::ReadPtr(entity + gHitMonOff);
        if (mon != gMonPtr) {
            gMonPtr   = mon;
            gMonHp    = -1.0f;
            gMonHpMax = -1.0f;
        }
        if (mon) {
            const std::uintptr_t hp = mem::ReadPtr(mon + gMonHpPtrOff);
            if (hp) {
                const float mx = mem::ReadF32(hp + gMonHpMaxOff, -1.0f);
                const float cu = mem::ReadF32(hp + gMonHpCurOff, -1.0f);
                // 上限超出保护范围 = 读到别的结构了（换区/加载中的垃圾值），丢弃
                if (mx >= 0.0f && mx <= gMonHpLimit) {
                    gMonHpMax = mx;
                    gMonHp    = cu;
                }
            }
        }
    }

    // ---- 场景/装备稳定检测 ----
    // entity 已存在且武器读到了：若武器与上次不同（= 加载/切装完成），
    // 或还没初始化过，就把"稳定起点"推到当前时刻。之后 kWemSettleMs 内的
    // wem 会被 WemEventSink 忽略（压制进图预加载的假触发）。
    if (gWeapon >= 0 && gWeapon != gLastWeaponStable) {
        gLastWeaponStable = gWeapon;
        gStableSinceMs    = ::GetTickCount64();
    }
}

// true when the player entity exists (in-scene; used before touching chat memory)
bool RefreshIsInScene()
{
    std::uintptr_t manager = 0;
    if (!mem::ReadVal(gRoot, manager) || manager == 0) return false;
    std::uintptr_t entity = 0;
    const std::uint32_t c0[1] = {0x50};
    entity = mem::Walk(manager, c0, 1);
    return entity != 0;
}

} // namespace player

// ===========================================================================
//  合并进来的 Wwise 捕获模块的对外接口（定义在 src\wwise\SonarAudio.cpp）
//  必须放全局作用域：那个模块在全局 namespace sonaraudio 里。
// ===========================================================================
namespace sonaraudio {
void SonarAudioStart(HMODULE m);
void SonarAudioStop();
void SetEventSink(void (*sink)(uint32_t media_id, uint32_t event_id, uint32_t playing_id,
                               const char* bank, const char* name));
}

// ===========================================================================
//  Module paths + logging
// ===========================================================================
namespace plugin {

HMODULE gModule = nullptr;
volatile LONG gStop = 0;
std::wstring gModuleDir;   // DLL 所在目录（ends with '\'）
std::wstring gDataDir;     // 数据目录：<ModuleDir>WeaponSoundEnhance\ ，旧布局回退到 ModuleDir
std::wstring gIniPath;
std::wstring gLogPath;
volatile int gDebug = 0;   // ini Debug=1 enables per-play / heartbeat logging

void Log(const char* fmt, ...);

// 长路径：Win32 默认受 MAX_PATH(260) 限制，插件装在很深的目录（mod 管理器/长文件夹名）
// 时会读不到 ini/wav。加 \\?\ 前缀可以突破（要求绝对路径且只用反斜杠）。
std::wstring LongPathW(const std::wstring& p)
{
    if (p.size() < 250) return p;
    std::wstring w = p;
    for (auto& c : w) if (c == L'/') c = L'\\';
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    if (w.size() >= 2 && w[1] == L':') return L"\\\\?\\" + w;
    return w;
}

// 文件是否存在
bool FileExistsW(const std::wstring& p)
{
    const DWORD a = ::GetFileAttributesW(LongPathW(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 目录是否存在
bool DirExistsW(const std::wstring& p)
{
    const DWORD a = ::GetFileAttributesW(LongPathW(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 选数据目录，规则（兼容旧布局）：
//   1) <plugins>\WeaponSoundEnhance\WeaponSoundEnhance.ini 存在 → 用子目录
//   2) <plugins>\WeaponSoundEnhance.ini 存在              → 用旧布局(DLL 同目录)
//   3) 都没有 → 建子目录并用子目录（新装用户）
std::wstring ResolveDataDir()
{
    const std::wstring sub = gModuleDir + L"WeaponSoundEnhance\\";
    if (FileExistsW(sub + L"WeaponSoundEnhance.ini")) return sub;
    if (FileExistsW(gModuleDir + L"WeaponSoundEnhance.ini")) return gModuleDir;
    ::CreateDirectoryW(sub.c_str(), nullptr);
    return sub;
}

// 用户 ini 不存在时，用随包模板生成一份（这样更新只覆盖模板、不会覆盖用户配置）
void SeedIniFromTemplate()
{
    if (FileExistsW(gIniPath)) return;
    const std::wstring tpl = gDataDir + L"WeaponSoundEnhance.ini.template";
    if (!FileExistsW(tpl)) return;
    if (::CopyFileW(tpl.c_str(), gIniPath.c_str(), FALSE)) {
        Log("created WeaponSoundEnhance.ini from template");
    }
}

void LogInit()
{
    gLogPath = gDataDir + L"WeaponSoundEnhance.log";
    ::DeleteFileW(gLogPath.c_str());
    Log("WeaponSoundEnhance 2.18 starting");
    // 旧布局提示：wav 还在 plugins\sounds\ 时自动兼容，但建议搬进数据目录
    const std::wstring oldSounds = gModuleDir + L"sounds";
    if (gDataDir != gModuleDir && DirExistsW(oldSounds) &&
        !DirExistsW(gDataDir + L"sounds")) {
        Log("hint: 音效目录仍在旧位置 %s （已自动兼容；建议整体移动到 %s ）",
            strconv::ToUtf8(oldSounds).c_str(),
            strconv::ToUtf8(gDataDir + L"sounds").c_str());
    }
}

void LogV(const char* fmt, va_list ap)
{
    char buf[2048] = {};
    vsnprintf_s(buf, _TRUNCATE, fmt, ap);
    ::OutputDebugStringA(buf);
    FILE* f = nullptr;
    if (_wfopen_s(&f, LongPathW(gLogPath).c_str(), L"ab") == 0 && f) {
        SYSTEMTIME st{}; ::GetLocalTime(&st);
        fprintf(f, "[%02u:%02u:%02u.%03u] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
}

void Log(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

// verbose logging, only active when Debug=1
void LogD(const char* fmt, ...)
{
    if (!gDebug) return;
    va_list ap; va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

} // namespace plugin

// ===========================================================================
//  Standalone WAV player: winmm waveOut, one handle per voice, concurrency cap
// ===========================================================================
namespace audio {

struct Wav
{
    std::vector<std::uint8_t> data;   // PCM payload only (RIFF header stripped)
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPer = 0;
    bool valid = false;
};

bool ParseWav(const std::uint8_t* buf, std::size_t size, Wav& out)
{
    out = Wav{};
    if (!buf || size < 44) return false;
    if (std::memcmp(buf, "RIFF", 4) != 0 || std::memcmp(buf + 8, "WAVE", 4) != 0)
        return false;

    std::uint16_t tag = 0, ch = 0, bits = 0;
    std::uint32_t rate = 0;
    const std::uint8_t* dataPtr = nullptr;
    std::uint32_t dataLen = 0;
    bool haveFmt = false, haveData = false;

    std::size_t pos = 12;
    while (pos + 8 <= size) {
        char id[5] = {0};
        std::memcpy(id, buf + pos, 4);
        std::uint32_t chunk = 0;
        std::memcpy(&chunk, buf + pos + 4, 4);
        if ((std::size_t)chunk > size - (pos + 8)) break;

        if (std::memcmp(id, "fmt ", 4) == 0 && chunk >= 16) {
            std::memcpy(&tag,  buf + pos + 8 + 0, 2);
            std::memcpy(&ch,   buf + pos + 8 + 2, 2);
            std::memcpy(&rate, buf + pos + 8 + 4, 4);
            std::memcpy(&bits, buf + pos + 8 + 14, 2);
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataPtr = buf + pos + 8;
            dataLen = chunk;
            haveData = true;
        }
        pos += 8 + chunk;
        if (chunk & 1) pos += 1;
    }

    if (!haveFmt || !haveData || tag != 1 || ch == 0 || rate == 0 || !dataPtr || dataLen == 0)
        return false;

    out.channels = ch; out.sampleRate = rate; out.bitsPer = bits;
    out.data.assign(dataPtr, dataPtr + dataLen);
    out.valid = true;
    return true;
}

// 解码 wav / mp3 / ogg / flac 到 16-bit PCM。保留原始采样率，
// 常见采样率(44100/48000 等)不重采样，保证最大音质。
bool DecodeAudio(const std::uint8_t* buf, std::size_t size,
                 std::string& fmtName, Wav& out)
{
    out = Wav{};
    wseaudio::Pcm p;
    if (!wseaudio::DecodeFileBytes(buf, size, p) || !p.valid || p.samples.empty())
        return false;
    fmtName = wseaudio::FormatName(p.format);
    out.channels = (std::uint16_t)p.channels;
    out.sampleRate = p.rate;
    out.bitsPer = 16;
    out.data.resize(p.samples.size() * 2);
    std::memcpy(out.data.data(), p.samples.data(), p.samples.size() * 2);
    out.valid = true;
    return true;
}

// Linear-interpolation resample of 16-bit PCM to a standard rate (waveOut
// rejects exotic rates such as 28000 Hz; 44100 is the playback standard).
void ResampleTo(Wav& w, std::uint32_t targetRate = 44100)
{
    if (!w.valid || w.sampleRate == targetRate) return;
    if (w.bitsPer != 16) return;

    const std::size_t inSamples = w.data.size() / 2;
    if (inSamples == 0) return;
    const std::size_t outSamples = (std::size_t)((double)inSamples * targetRate / w.sampleRate);
    if (outSamples == 0) return;

    const std::int16_t* in = reinterpret_cast<const std::int16_t*>(w.data.data());
    const double step = (double)inSamples / (double)outSamples;
    std::vector<std::int16_t> out(outSamples);
    for (std::size_t i = 0; i < outSamples; ++i) {
        double pos = (double)i * step;
        std::size_t idx = (std::size_t)pos;
        if (idx >= inSamples - 1) idx = inSamples - 1;
        double frac = pos - (double)idx;
        const std::size_t nxt = (idx + 1 < inSamples) ? idx + 1 : idx;
        double v = in[idx] * (1.0 - frac) + in[nxt] * frac;
        out[i] = (std::int16_t)v;
    }
    w.data.resize(out.size() * 2);
    std::memcpy(w.data.data(), out.data(), out.size() * 2);
    w.sampleRate = targetRate;
}

// ---------------------------------------------------------------------------
//  声部管理：把"满了直接丢新的"改成"满了抢最旧的"
//
//  背景（用户实测）：5 个固定音效 + 1 个随机层 = 6 个同时发声，正好撞上原来的
//  上限 kMaxVoices=6 → 第 6 个被**静默丢弃**，表现就是"最后一个固定音效时响时不响"。
//  现在：上限提高，且满了先把最旧的声部打断，保证**新的这次一定响**。
// ---------------------------------------------------------------------------
const int kMaxVoices = 24;   // 同时发声上限（原来 6：5 固定 + 随机层就撞上限）

struct VoiceEntry {
    std::uint64_t id = 0;
    std::uint64_t startMs = 0;
    std::shared_ptr<std::atomic<bool>> kill;   // 置 true = 立刻打断这个声部
};
std::mutex gVoiceMx;
std::vector<VoiceEntry> gVoiceList;
std::atomic<std::uint64_t> gVoiceSeq{ 0 };
std::atomic<int> gVoices{ 0 };   // 当前活跃声部计数（VoiceGuard 维护，日志/统计用）

std::shared_ptr<std::atomic<bool>> AcquireVoice()
{
    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lk(gVoiceMx);
    if ((int)gVoiceList.size() >= kMaxVoices && !gVoiceList.empty()) {
        std::size_t oldest = 0;
        for (std::size_t i = 1; i < gVoiceList.size(); ++i)
            if (gVoiceList[i].startMs < gVoiceList[oldest].startMs) oldest = i;
        if (gVoiceList[oldest].kill) gVoiceList[oldest].kill->store(true);
        plugin::LogD("[voice] cap %d reached: stealing oldest voice (不再丢新音效)", kMaxVoices);
        gVoiceList.erase(gVoiceList.begin() + oldest);
    }
    VoiceEntry e;
    e.id = ++gVoiceSeq;
    e.startMs = ::GetTickCount64();
    e.kill = flag;
    gVoiceList.push_back(e);
    return flag;
}

void ReleaseVoice(const std::shared_ptr<std::atomic<bool>>& flag)
{
    std::lock_guard<std::mutex> lk(gVoiceMx);
    for (std::size_t i = 0; i < gVoiceList.size(); ++i)
        if (gVoiceList[i].kill == flag) { gVoiceList.erase(gVoiceList.begin() + i); break; }
}

int ActiveVoices()
{
    std::lock_guard<std::mutex> lk(gVoiceMx);
    return (int)gVoiceList.size();
}

// 停止信号：StopAll() 每次 +1；正在播放的声部如果发现自己的起始代次和当前不符，
// 立刻 waveOutReset 打断。用于「命中某派生动作就停止播放」的 Stop 条目。
std::atomic<std::uint64_t> gStopEpoch{0};

void StopAll()
{
    gStopEpoch.fetch_add(1, std::memory_order_relaxed);
}

struct Engine
{
    bool Init()
    {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return true;
    }

    // Copy PCM with per-sound gain, then hand to a worker thread. Returns true
    // once the copy was accepted; the voice cap is enforced inside the worker.
    bool Play(const Wav& w, float volume, unsigned delayMs = 0, unsigned* durMs = nullptr)
    {
        if (!w.valid || w.data.empty()) { mLastHr = E_INVALIDARG; return false; }
        if (w.bitsPer != 16) { mLastHr = E_INVALIDARG; return false; }

        PlayCtx* ctx = new PlayCtx();
        const std::size_t n = w.data.size() / 2;
        ctx->samples.resize(n);
        const std::int16_t* src = reinterpret_cast<const std::int16_t*>(w.data.data());
        float gain = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
        for (std::size_t i = 0; i < n; ++i)
            ctx->samples[i] = (std::int16_t)(src[i] * gain);
        ctx->channels = w.channels;
        ctx->rate = w.sampleRate;
        ctx->bits = w.bitsPer;
        ctx->delayMs = delayMs;
        if (durMs) {
            const unsigned ch = ctx->channels ? ctx->channels : 1;
            const unsigned rate = ctx->rate ? ctx->rate : 44100;
            *durMs = (unsigned)((ctx->samples.size() / ch) * 1000ull / rate);
        }

        HANDLE th = ::CreateThread(nullptr, 0, &PlayWorker_Host, ctx, 0, nullptr);
        if (!th) { delete ctx; mLastHr = E_OUTOFMEMORY; return false; }
        ::CloseHandle(th);
        return true;
    }

    HRESULT LastHr() const { return mLastHr; }

private:
    struct PlayCtx {
        std::vector<std::int16_t> samples;
        WORD  channels = 2;
        DWORD rate = 44100;
        WORD  bits = 16;
        unsigned delayMs = 0;
    };

    struct VoiceGuard {
        VoiceGuard() { ++gVoices; }
        ~VoiceGuard() { --gVoices; }
    };

    static DWORD WINAPI PlayWorker_Host(LPVOID param)
    {
        std::unique_ptr<PlayCtx> ctx(static_cast<PlayCtx*>(param));
        // ★ 不再"满了丢新的"：登记一个声部，满了会先打断最旧的（见 AcquireVoice）
        auto kill = AcquireVoice();
        VoiceGuard vg;
        PlayWorker(*ctx, kill);
        ReleaseVoice(kill);
        return 0;
    }

    static void PlayWorker(PlayCtx& ctx, const std::shared_ptr<std::atomic<bool>>& kill)
    {
        if (ctx.samples.empty()) return;
        const std::uint64_t epoch = gStopEpoch.load(std::memory_order_relaxed);
        // 延时期间也可被 Stop 打断（比如蓄力音效的延时还没到就出了下一招）
        if (ctx.delayMs > 0) {
            for (unsigned waited = 0; waited < ctx.delayMs; waited += 10) {
                ::Sleep(10);
                if (kill->load() || gStopEpoch.load(std::memory_order_relaxed) != epoch) return;
            }
        }

        WAVEFORMATEX wfx = {};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = ctx.channels;
        wfx.nSamplesPerSec = ctx.rate;
        wfx.wBitsPerSample = ctx.bits;
        wfx.nBlockAlign = (WORD)((ctx.bits / 8) * ctx.channels);
        wfx.nAvgBytesPerSec = ctx.rate * wfx.nBlockAlign;

        HWAVEOUT hwo = nullptr;
        // ★ 插拔音频设备（耳机/USB 声卡）后设备会短暂不可用：重试几次再放弃，
        //   否则用户看到的就是"有时候完全没声音"（且没有任何提示）。
        MMRESULT mo = MMSYSERR_NOERROR;
        for (int attempt = 0; attempt < 3; ++attempt) {
            mo = waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
            if (mo == MMSYSERR_NOERROR) break;
            ::Sleep(120);
        }
        if (mo != MMSYSERR_NOERROR || !hwo) {
            plugin::LogD("[audio] waveOutOpen 失败(0x%08X)：设备可能刚被插拔；改用 PlaySound 兜底", (unsigned)mo);
            // 兜底：合成一个内存 WAV 交给 PlaySound（它自己处理设备变化，成功率更高）
            std::vector<std::uint8_t> wav;
            wav.reserve(44 + ctx.samples.size() * 2);
            const std::uint32_t dataLen = (std::uint32_t)(ctx.samples.size() * 2);
            const std::uint32_t riffLen = 36 + dataLen;
            const std::uint16_t blockAlign = (std::uint16_t)((ctx.bits / 8) * ctx.channels);
            const std::uint32_t byteRate = ctx.rate * blockAlign;
            auto put32 = [&wav](std::uint32_t v) { for (int k = 0; k < 4; ++k) wav.push_back((std::uint8_t)(v >> (8 * k))); };
            auto put16 = [&wav](std::uint16_t v) { for (int k = 0; k < 2; ++k) wav.push_back((std::uint8_t)(v >> (8 * k))); };
            wav.push_back('R'); wav.push_back('I'); wav.push_back('F'); wav.push_back('F');
            put32(riffLen);
            wav.push_back('W'); wav.push_back('A'); wav.push_back('V'); wav.push_back('E');
            wav.push_back('f'); wav.push_back('m'); wav.push_back('t'); wav.push_back(' ');
            put32(16); put16(1); put16(ctx.channels); put32(ctx.rate); put32(byteRate);
            put16(blockAlign); put16(ctx.bits);
            wav.push_back('d'); wav.push_back('a'); wav.push_back('t'); wav.push_back('a');
            put32(dataLen);
            const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(ctx.samples.data());
            wav.insert(wav.end(), src, src + dataLen);
            ::PlaySoundW(nullptr, nullptr, 0);
            ::PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
            return;
        }
        std::vector<std::uint8_t> bytes(ctx.samples.size() * 2);
        std::memcpy(bytes.data(), ctx.samples.data(), bytes.size());

        WAVEHDR hdr = {};
        hdr.lpData = reinterpret_cast<LPSTR>(bytes.data());
        hdr.dwBufferLength = static_cast<DWORD>(bytes.size());
        if (waveOutPrepareHeader(hwo, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) {
            waveOutClose(hwo); return;
        }
        waveOutWrite(hwo, &hdr, sizeof(hdr));
        while ((hdr.dwFlags & WHDR_DONE) == 0) {
            ::Sleep(10);
            if (kill->load() || gStopEpoch.load(std::memory_order_relaxed) != epoch) {
                // 被 Stop / 被"抢声部"打断：waveOutReset 让当前缓冲立刻完成
                ::waveOutReset(hwo);
                while ((hdr.dwFlags & WHDR_DONE) == 0) ::Sleep(5);
                break;
            }
        }
        waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr));
        waveOutClose(hwo);
    }

    HRESULT mLastHr = S_OK;
};

Engine g_audio;

} // namespace audio

// ===========================================================================
//  Config model + ini parsing
// ===========================================================================
namespace plugin {

// --- global switches (also toggled by hotkeys / chat commands) ---
std::uintptr_t gPlayerRoot = 0x1450139A0ULL;
int gPollMs = 60;
int gDebounceMs = 120;
volatile int gVolumePct = 100;
volatile int gEnabled = 1;
volatile int gMoreSounds = 1;   // legacy: only affects pools without any fixed sound
volatile int gEfxCapture = 0;    // 特效捕获（实验性）：默认关闭 —— 不开就不扫描内存、不挂钩，零开销
volatile int gEfxLogAll = 0;     // 诊断开关（仅 EfxCapture=1 时有效）
volatile int gEfxDiscover = 0;  // 诊断开关（仅 EfxCapture=1 时有效）
std::string   gEfxTablePref;     // ini: EfxTablePref=3=hm_wp03_25（同一 (组,记录) 在多个变体表里都有时用）
int EfxWeaponProvider() { return player::gWeapon; }   // 给 sonarefx 用：当前武器类型（挑表用）

// --- wem 误触发抑制（进图/换区预加载）---
//   KillSettleMs: 进图/切装完成后，多少毫秒内的 wem 一律忽略（0 = 关闭该抑制）
//   WemBurstCap : 同一 60ms 窗口内超过多少条 wem 就判为"预加载整批丢弃"（0 = 关闭）
int gKillSettleMs = 2500;
int gWemBurstCap  = 6;

// --- 循环音过滤（不是动作事件、却持续请求的 wem，例：wp_bow_cmn/10 每秒一次）---
//   WemLoopFilter: 1 = 开（默认）
//   WemLoopRate  : 同一 media 在 1 秒内达到这个次数即判为循环音，本秒内后续全忽略
int gWemLoopFilter = 1;
int gWemLoopRate   = 6;      // 3 秒窗口内达到这个次数即判为循环音（放宽：正常动作别被误判）
int gWemLoopMuteMs = 4000;   // 判定为循环音后，忽略该 media 的时长（毫秒）

// --- 武器触发开关（每武器一个，默认全关）---
//   WeaponEnabled=0,1,...（14 个逗号分隔的 0/1，对应武器 0..13；缺省按 0）
//   某个武器没开 → 该武器的条目（含通用条目按当前持握武器判定）一律不触发。
bool gWeaponEnabled[14] = {};

// --- wem 场景过滤（MHW_Memory_Map_Skill.md §3，已实测文档）---
//   WemOnlyInQuest : 1 = 仅任务进行中(QuestState==2)才触发 wem（压掉集会/换区/菜单误触发）
//   WemMapWhite    : 逗号分隔地图 id；填了则只在这些地图触发（进图后在派生流看 mapId 填）
//   WemMapBlack    : 逗号分隔地图 id；这些地图不触发（白名单优先）
int gWemOnlyInQuest = 0;
std::vector<int> gWemMapWhite, gWemMapBlack;

static bool WemScenePermitted() {
    if (gWemOnlyInQuest && player::gQuestState != 2)
        return false;
    if (!gWemMapWhite.empty()) {
        for (int m : gWemMapWhite) if (m == player::gMapId) return true;
        return false;
    }
    if (!gWemMapBlack.empty()) {
        for (int m : gWemMapBlack) if (m == player::gMapId) return false;
    }
    return true;
}
// 判断"当前应不应该触发该武器的音效"：weaponType<0（通用）按当前武器走
static inline bool WeaponPermitted(int weaponType, int curWeapon) {
    const int w = (weaponType >= 0) ? weaponType : curWeapon;
    if (w < 0 || w > 13) return true;   // 拿不到武器信息时不禁（保持 raw wem 路径可用）
    return gWeaponEnabled[w];
}

// 永久忽略的 media 名单（ini: WemIgnoreMedia=182089195,xxx）。
// 用于"很多行为都会触发"的持续音（行为判定也可能漏），直接点名忽略。
std::vector<long long> gWemIgnoreMedia;

bool WemIsIgnored(long long media) {
    for (std::size_t i = 0; i < gWemIgnoreMedia.size(); ++i)
        if (gWemIgnoreMedia[i] == media) return true;
    return false;
}
std::uint32_t gGaugePtrOff = 0x76B0;
std::uint32_t gGaugeValOff = 0x2370;
std::uint32_t gChargeValOff = 0x2358;          // 大剑蓄力等级（与刃级共用 GaugePtrOff）
std::uint32_t gFsmTargetOff = 0x6274;          // FSM target 值偏移
std::uintptr_t gQuestRoot   = 0x14500ED30ULL;  // 任务结构入口
std::uint32_t gQuestDmgOff  = 0x17088;         // 任务累计伤害（只含你自己）

// 命中判定的备用信号：怪物血量（QuestDmg 读不到时用）
std::uint32_t gHitMonOff   = 0x12958;   // *(entity + off) = 武器最近命中的怪物
std::uint32_t gMonHpPtrOff = 0x7670;    // *(monster + off) = 生命对象
std::uint32_t gMonHpMaxOff = 0x60;      // float 上限
std::uint32_t gMonHpCurOff = 0x64;      // float 当前值

volatile int g_useChatEcho = 1;
volatile int g_useChatCommands = 1;
volatile int g_hotkeysEnabled = 1;   // 启用/关闭热键（ini [WeaponSoundEnhance] Hotkeys=0）

// ---------------------------------------------------------------------------
//  监听模式（WatchMode）—— 把"捕获到的东西"分别显示出来
//
//  这是作者的调试/勘察开关：Sonar 的玩法靠 FSM/LMT 匹配来触发音效，
//  但想知道"某个派生到底叫什么 ID、最终调用了哪个 wem"时，光看配置很费劲。
//  打开它就能在日志里看到两条独立的流：
//
//    位 0 (1) = **派生 ID 流**：FSM / FSM层 / LMT 逐帧变化，变化时才打
//                （附当前武器与动作名），用来查一个动作的真实 ID。
//    位 1 (2) = **WEM 调用流**：每次真正播放音效时，打出来自哪条条目、
//                哪个 wem 文件、音量，用来核对"ID 匹配上以后到底响了什么"。
//
//  组合：0=关（默认，零开销） 1=只看派生 2=只看 wem 3=两者都看
//  运行时可聊天命令 `/wse watch 0..3` 热切换，或 ini `WatchMode=`。
// ---------------------------------------------------------------------------
volatile int gWatchMode = 0;

// 派生 ID 流的边沿检测状态（仅主循环线程访问，无需线程安全）
int gWatchLastFsm = -2, gWatchLastFsmTgt = -2, gWatchLastLmt = -2, gWatchLastWeapon = -2;

// ---------------------------------------------------------------------------
//  wem 触发通道：读 SonarAudio.log（另一只 agent 的 hook 输出，与 DLL 同在
//  plugins\ 下）的增量事件，按条目的 Media= 匹配播放我们的音效。
//  与 FSM/LMT 判定完全独立、可同时生效。
// ---------------------------------------------------------------------------
std::ifstream gWemStream;
std::wstring  gWemLogPath;
std::uint64_t gWemLastPollMs = 0;
std::uint64_t gWemLastSize = 0;       // 文件大小跟踪：重建/截断检测（防重放）
long long     gWemLastId = -1;      // 同 media 防抖：300ms 内不重复触发
std::uint64_t gWemLastPlayMs = 0;

int  WemWeaponFromBank(const std::string& bank);   // 定义见下（需要 gAttacks 声明之后）
void PollWemEvents(std::mt19937& rng);             // 定义见下

// ---------------------------------------------------------------------------
//  合并进来的 Wwise 捕获模块（原 SonarAudio.dll，源码在 src\wwise\）：
//  解析 exe 导出表拿 PostEvent → MinHook 三 hook → 扫 nbnk 建 event→media 反查
//  → 事件经 SetEventSink 回调进来，直接触发 wem 条目。
//  （不再需要单独的 SonarAudio.dll；它写的 SonarAudio.log 仍保留给 GUI 读）
//
//  注意：声明放在全局作用域（该模块本身在全局 namespace sonaraudio 里，
//  不能写进 plugin 里，否则符号名变成 plugin::sonaraudio::*）。见文件顶部。
// ---------------------------------------------------------------------------

// 是否启用"读磁盘日志"兜底通道（hook 生效时关掉，避免同一事件触发两次）
std::atomic<bool> g_useLogWemFallback{false};

// ---------------------------------------------------------------------------
//  ★ layer 探针：装瓶/平射这类动作在"层"(layer) 上，主状态字段(entity+0x6278)
//  读不到。做法：wem 事件（武器 bank）到达时"武装"，下一帧把玩家实体关键区段
//  与上一快照对比，把**变化的字段**写进 WeaponSoundEnhance_layer.log。
//  做几次装瓶/解瓶就能从变化里认出 layer 的 fsm/lmt 字段。
//  只在 ini LayerProbe=1 时工作（默认关，零开销）。
// ---------------------------------------------------------------------------
std::atomic<bool> g_layerProbeArmed{false};
std::atomic<int>  g_layerProbeMedia{-1};
int               gLayerProbeEnabled = 0;   // ini LayerProbe=1 打开（默认关）

// 武装探针（由 WemEventSink 在武器 bank 事件时调用）
void ArmLayerProbe(int media) {
    g_layerProbeMedia.store(media, std::memory_order_relaxed);
    g_layerProbeArmed.store(true, std::memory_order_release);
}

// ★ 事件出口（实现见文件后部 FireEntry 之后：依赖 gAttacks / FireEntry）
void WemEventSink(uint32_t media_id, uint32_t event_id, uint32_t playing_id,
                  const char* bank, const char* name);

// --- hotkeys (defaults; ini [Hotkeys] overrides) ---
int gModifierKey = VK_CONTROL;
int gReloadKey   = VK_F5;
int gVolUpKey    = VK_UP;
int gVolDownKey  = VK_DOWN;
int gSetVolKey   = VK_F8;
int gToggleKey   = VK_F9;
int gMoreKey     = VK_F10;
int gComboKey    = VK_F11;   // 切换当前武器配置组合
int gSetVolValue = 50;

// --- game module addresses (15.23.00) ---
std::uintptr_t gGameBase = 0;
const std::uintptr_t kSystemMessageRva = 0x1A540D0;
const std::uintptr_t kSystemMessageMgrRva = 0x500CE70;
typedef void (*SystemMessageFn)(void* manager, const char* utf8Buffer,
                                float duration, std::int32_t messageId, bool emphasized);
SystemMessageFn g_systemMessage = nullptr;

const std::uintptr_t kMessageBaseRva = 0x4F87FF0;
const std::uintptr_t kMessageLenOff  = 0xBC;
const std::uintptr_t kMessageBodyOff = 0xC0;

// ===========================================================================
//  Data model
// ===========================================================================

// One selectable sound. path is relative to the module dir (usually "sounds/x.wav").
// ===========================================================================
//  条件表达式  ——  给「动作 + 判定结果」式的触发用
//
//    语法:  <变量> <比较符> <数字>，用 & (且) / | (或) 连接，& 优先级更高
//    例子:  dmg>0                      打出了伤害
//           dmg>0 & dAura>=0           打出伤害 且 没掉刃
//           dAura<0 | dmg==0           掉刃 或 完全没打中
//
//  不支持括号：两级优先级（若干 AND 组再 OR 起来）已经够覆盖实际需求，
//  而且解析简单、不容易写错。
// ===========================================================================
namespace cond {

enum VarId { V_DMG = 0, V_AURA, V_DAURA, V_CHARGE, V_DCHARGE,
             V_LMT, V_FSM, V_FSMTGT, V_MS, V_COUNT };

// 注意：查找时按长度从长到短匹配，否则 "dAura" 会被 "d" 之类的前缀吃掉
const char* const kVarNames[V_COUNT] = {
    "dmg", "aura", "dAura", "charge", "dCharge", "lmt", "fsm", "fsmTarget", "ms"
};

// 给 GUI / 文档用的中文名，顺序与 VarId 一致
const char* const kVarLabels[V_COUNT] = {
    "打出伤害", "练气/刃级", "练气变化", "蓄力等级", "蓄力变化",
    "动作ID", "FSM", "FSM层", "已过毫秒"
};

enum CmpOp { OP_GT = 0, OP_GE, OP_LT, OP_LE, OP_EQ, OP_NE, OP_COUNT };
const char* const kOpNames[OP_COUNT] = { ">", ">=", "<", "<=", "==", "!=" };

struct Vars { int v[V_COUNT]; };

struct Term {
    int  var = V_DMG;
    int  op  = OP_GT;
    int  rhs = 0;
    bool orBefore = false;   // 这一项之前的连接符是 |（false 表示 &）
};

struct Expr {
    std::vector<Term> terms;
    bool empty() const { return terms.empty(); }
};

inline std::string Trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

// 含比较符就当成表达式，否则交回旧的刃级 tag 解析（none/white/yellow/red/0..3）
inline bool LooksLikeExpr(const std::string& s)
{
    return s.find_first_of("<>=!&|") != std::string::npos;
}

inline bool ParseOne(const std::string& raw, Term& t)
{
    const std::string x = Trim(raw);
    if (x.empty()) return false;

    // 先定位比较符
    std::size_t opPos = x.find_first_of("<>=!");
    if (opPos == std::string::npos || opPos == 0) return false;

    std::string varName = Trim(x.substr(0, opPos));
    int varId = -1;
    for (int i = 0; i < V_COUNT; ++i) {
        if (varName == kVarNames[i]) { varId = i; break; }
    }
    if (varId < 0) return false;

    // 两字符的比较符优先
    int op = -1; std::size_t opLen = 0;
    const std::string rest = x.substr(opPos);
    for (int i = 0; i < OP_COUNT; ++i) {
        const std::size_t n = std::strlen(kOpNames[i]);
        if (n == 2 && rest.compare(0, 2, kOpNames[i]) == 0) { op = i; opLen = 2; break; }
    }
    if (op < 0) {
        for (int i = 0; i < OP_COUNT; ++i) {
            const std::size_t n = std::strlen(kOpNames[i]);
            if (n == 1 && rest.compare(0, 1, kOpNames[i]) == 0) { op = i; opLen = 1; break; }
        }
    }
    if (op < 0) return false;

    const std::string rhs = Trim(rest.substr(opLen));
    if (rhs.empty()) return false;
    for (std::size_t i = 0; i < rhs.size(); ++i) {
        if (i == 0 && (rhs[i] == '-' || rhs[i] == '+')) continue;
        if (rhs[i] < '0' || rhs[i] > '9') return false;
    }

    t.var = varId; t.op = op; t.rhs = std::atoi(rhs.c_str());
    return true;
}

// 解析失败返回 false（调用方会退回旧解析并记一条日志）
inline bool Parse(const std::string& src, Expr& out)
{
    out.terms.clear();
    std::string cur;
    bool nextIsOr = false;
    bool pendingOr = false;

    for (std::size_t i = 0; i <= src.size(); ++i) {
        const char c = (i < src.size()) ? src[i] : '\0';
        if (c == '&' || c == '|' || c == '\0') {
            Term t;
            if (!ParseOne(cur, t)) return false;
            t.orBefore = nextIsOr;
            out.terms.push_back(t);
            cur.clear();
            nextIsOr = (c == '|');
            pendingOr = nextIsOr;
            (void)pendingOr;
        } else {
            cur.push_back(c);
        }
    }
    return !out.terms.empty();
}

// 求值：若干 AND 组再 OR 起来
inline bool Eval(const Expr& e, const Vars& vars)
{
    if (e.terms.empty()) return false;
    bool result = false;   // 已经闭合的 OR 结果
    bool group  = true;    // 当前 AND 组
    for (std::size_t i = 0; i < e.terms.size(); ++i) {
        const Term& t = e.terms[i];
        if (i > 0 && t.orBefore) { result = result || group; group = true; }
        const int lhs = vars.v[t.var];
        bool ok = false;
        switch (t.op) {
            case OP_GT: ok = lhs >  t.rhs; break;
            case OP_GE: ok = lhs >= t.rhs; break;
            case OP_LT: ok = lhs <  t.rhs; break;
            case OP_LE: ok = lhs <= t.rhs; break;
            case OP_EQ: ok = lhs == t.rhs; break;
            case OP_NE: ok = lhs != t.rhs; break;
            default: break;
        }
        group = group && ok;
    }
    return result || group;
}

} // namespace cond

struct SoundSpec
{
    std::string path;
    int delay = 0;    // ms to wait before playback
    int vol = 100;    // 0..100, 100 = no extra gain beyond master volume
    bool fixed = false; // F flag: always plays when its pool is hit
    int  cdMs = 0;      // C<ms> flag: 自身再触发冷却（0 = 用默认 400ms）
    bool playLock = false; // P flag: 播放期间(含延时)不再被重复触发
    bool plain = true;  // pure legacy token (no |d|v|F) -> SoundDelay/SoundVol apply
};

struct Pool
{
    std::vector<SoundSpec> specs;

    bool empty() const { return specs.empty(); }
    bool HasAnyFixed() const
    {
        for (const auto& s : specs) if (s.fixed) return true;
        return false;
    }
};

// 一条「条件 -> 音效池」。按书写顺序求值，第一个成立的那条播。
struct CondPool
{
    std::string text;    // 原始表达式，写回 ini / 日志用
    cond::Expr  expr;
    Pool        pool;

    // false (Sound:)    条件一成立就播 —— 适合"已成定局"的条件，比如掉刃
    // true  (SoundEnd:) 只在窗口结束时评 —— 适合"还可能被推翻"的条件
    //
    // 这个区分是必须的：大居"不掉刃且打出伤害"在伤害到账的那一瞬间是成立的，
    // 但刃是稍后才掉的（实测 dmg=258 在 ms=297 到账，那时 dAura 还是 0）。
    // 用 Sound: 会在这一瞬间误判成功，必须 SoundEnd: 撑到窗口结束再评。
    bool atEnd = false;
};

// One ini [Attack...] section: trigger conditions + sound pools.
struct Attack
{
    int weaponType = -1;          // -1 = any weapon
    int fsmId = -1;               // -1 = any FSM
    int fsmTarget = -1;           // FSMTarget= 目标层；-1 = 不限定（只比 id，旧行为）
    long long media = -1;         // Media=：WWise media id >0 时按"游戏播放该 wem"触发，
                                  // 与 fsm/lmt 判定二选一（wem 捕获添加的条目）
    std::vector<int> lmt;         // empty = any LMT
    std::string name;             // Name= display label (ignored by matching)
    std::string group;            // Group= logical action: all members fire at most
                                  // once per action occurrence (first trigger wins)
    Pool defPool;                 // default pool (tag = "any")
    Pool gaugePool[4];            // LS gauge pools, keyed by level 0..3
    bool stop = false;            // Stop=1：命中这条时停止正在播放的音效（无音效条目）
    bool inMatch = false;         // edge latch: fire once per action (ungrouped entries)
    std::uint64_t lastTriggerMs = 0;  // 每条目独立防抖：以前是全局 gLastTrigger，
                                       // 导致"同一条件写多条只有第一条响"、"紧跟上一招的动作被吃掉"

    // ---- 延迟判定（CheckTimeoutMs > 0 时启用）----
    // 动作匹配上只是"开窗"，接着盯一段时间，按条件表达式挑音效池。
    // 用来做"打中/落空""掉刃/升刃"这类**必须观察一段时间才知道结果**的触发。
    int checkDelayMs   = 0;       // 从第几毫秒开始计伤害（排除招式前段的伤害）
    int checkTimeoutMs = 0;       // 窗口长度；0 = 不启用延迟判定，行为与旧版完全一致
    int checkMode      = 0;       // 0=first 条件一成立立刻播  1=final 窗口结束再评一次
    // 窗口靠什么结束：0=只看时间(CheckTimeoutMs)  1=动作结束就算(CheckEndOn=action)
    // action 模式下动作离开本条目的 LMT 集合即视为结束——放完了、被打断、被派生
    // 掉都算，比拍一个固定时长准得多。CheckTimeoutMs 退居为保险上限。
    int checkEndOn     = 0;
    // 判定时刻的安全余量，**只能是正数或 0**（键名 CheckOffsetMs，老名 CheckGraceMs 仍认）。
    //
    // 判定时刻 = min(历史最晚出伤时刻 + offset, 动作结束 + offset)，再被 CheckTimeoutMs 封顶。
    //
    // "历史最晚出伤时刻"(maxDmgMs)是插件自己量出来的、跨窗口取最大值：过了这个点
    // 就不可能再有伤害，没必要陪着招式尾部的后摇干等。实测登龙的伤害 ms≈235 到账、
    // 动作却要跑到 1700~2400ms，靠这条能把判定提前一个数量级。
    //
    // 【为什么不要负偏移】早先支持过负数，用来"提前多少毫秒判"——那本质上是
    // 在不知道最晚出伤时刻的情况下靠动作时长反推。现在既然能直接量到，这个推算
    // 就多余了，而且有害：它让同一个键对不同招式要填相反的符号（大居要负、
    // 登龙要正），还出过"offset 比最晚出伤时刻还大 → 算出负数 → 一开窗就判失败"
    // 的事故。offset 现在只有一个含义：在量到的时刻之上留多少余量。
    //
    // 对打断免疫：maxDmgMs 取的是最大值，动作被提前打断只会让这一次不更新它，
    // 不会把它拉小。（早先按"上次动作时长"推算就没有这个性质。）
    int checkGraceMs   = 0;
    std::vector<CondPool> conds;  // 按书写顺序求值，第一个成立的播；都不成立走 defPool

    // ---- 运行时状态（不来自 ini）----
    bool          winOpen   = false;
    std::uint64_t winStart  = 0;
    bool          baseTaken = false;   // 是否已过 checkDelayMs、重取过伤害基准
    bool          actEnded  = false;   // 动作是否已经离开本条目（EndOn=action 用）
    std::uint64_t actEndAt  = 0;
    int           maxDmgMs  = -1;      // 历史上最晚一次出伤的时刻(ms)，跨窗口累积
    // 调试用：把窗口内每一笔伤害的到账时刻记下来，用来量"这一招分几段、各在第几毫秒"。
    // 多段攻击（大剑真蓄、太刀大居）必须靠这个才能把 CheckDelayMs 卡在两段中间。
    int           dmgPrev   = -1;      // 上一轮的窗口内累计伤害
    int           dmgHits   = 0;       // 已经记了几笔，防刷屏
    int           winDmg0   = 0;       // 计伤起点
    int           baseAura  = -1;
    int           baseCharge= -1;
    // 血量信号基准（QuestDmg 读不到时用它判 dmg）
    float         winHp0    = -1.0f;   // 计伤起点的怪物血量
    int           hpDmg     = 0;       // 窗口内累计掉血（血量信号口径，向上取整）
#ifdef WSE_PROBE_NO_MAIN
    int           lastDmg   = -99999;  // 测试探针用：记录最后一次算出的 V_DMG
#endif

    bool HasSounds() const
    {
        if (!defPool.empty()) return true;
        for (int i = 0; i < 4; ++i) if (!gaugePool[i].empty()) return true;
        for (const auto& c : conds) if (!c.pool.empty()) return true;
        return false;
    }
};

// 0..3 -> names used by Sound:<tag> keys
const char* const kGaugeNames[4] = {"none", "white", "yellow", "red"};

int GaugeTagToLevel(const std::string& tag)
{
    if (tag.empty()) return -1;
    if (tag.size() == 1 && tag[0] >= '0' && tag[0] <= '3') return tag[0] - '0';
    for (int i = 0; i < 4; ++i) {
        std::string n = kGaugeNames[i];
        if (n == tag) return i;
    }
    return -1;
}

std::vector<Attack> gAttacks;
std::mutex gCfgMutex;

// 前向声明（gLastTrigger / FireEntry 定义在其后）
extern std::uint64_t gLastTrigger;
bool FireEntry(const Attack& e, int gauge, std::mt19937& rng, std::uint64_t nowMs,
               const std::string& tag);

// ---------------------------------------------------------------------------
//  wem 触发通道（定义）—— bank 名 → 武器类型
// ---------------------------------------------------------------------------
int WemWeaponFromBank(const std::string& bank)
{
    if (bank.size() < 4 || ((bank[0] != 'w') && (bank[0] != 'W')) ||
                           ((bank[1] != 'p') && (bank[1] != 'P')))
        return -1;
    std::size_t i = 2;
    int v = 0;
    while (i < bank.size() && bank[i] >= '0' && bank[i] <= '9') { v = v * 10 + (bank[i] - '0'); ++i; }
    if (i > 2) return (v >= 0 && v <= 13) ? v : -1;
    std::string low = bank;
    for (auto& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    static const struct { const char* pre; int w; } tab[] = {
        { "wp_hbg_", 12 }, { "wp_lbg_", 13 }, { "wp_bowgun", 12 },
        { "wp_bow_", 11 }, { "wp_two_", 0 },  { "wp_one_", 1 },
        { "wp_sou_", 2 },  { "wp_swo_", 3 },  { "wp_ham_", 4 },
        { "wp_hue_", 5 },  { "wp_lan_", 6 },  { "wp_gun_", 7 },
        { "wp_saxe_", 8 }, { "wp_caxe_", 9 }, { "wp_rod_", 10 },
    };
    for (const auto& t : tab)
        if (low.compare(0, std::strlen(t.pre), t.pre) == 0) return t.w;
    return -1;
}

// ---------------------------------------------------------------------------
//  layer 探针实现：把 entity+0x6200..0x6800 与上次快照对比，输出变化的字段
//  （装瓶/平射的真实动作状态应该落在这里的某个偏移上）
// ---------------------------------------------------------------------------
void DumpLayerProbe() {
    if (!g_layerProbeArmed.exchange(false, std::memory_order_acq_rel)) return;
    if (!gLayerProbeEnabled) return;
    const std::uintptr_t ent = player::gEntity;
    if (!ent) return;

    constexpr std::uintptr_t kBase = 0x6200;
    constexpr std::size_t    kSpan = 0x600;      // 0x6200..0x6800，含 0x6278
    static std::vector<int> s_prev;
    std::vector<int> cur(kSpan / 4, 0);
    for (std::size_t i = 0; i < cur.size(); ++i)
        cur[i] = mem::ReadI32(ent + kBase + i * 4, 0);

    const int media = g_layerProbeMedia.load();
    if (!s_prev.empty()) {
        char buf[2048] = {};
        int n = _snprintf_s(buf, _TRUNCATE,
                            "[layer] media=%d fsm=%d fsmtgt=%d lmt=%d weapon=%d | 变化:",
                            media, player::gFsm, player::gFsmTarget, player::gLmt, player::gWeapon);
        int changes = 0;
        for (std::size_t i = 0; i < cur.size() && n > 0 && n < (int)sizeof(buf) - 40; ++i) {
            if (cur[i] == s_prev[i]) continue;
            n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, " +0x%llX:%d->%d",
                             (unsigned long long)(kBase + i * 4), s_prev[i], cur[i]);
            if (++changes >= 40) break;
        }
        if (changes) Log("%s", buf);
    }
    s_prev.swap(cur);
}

// 增量读 SonarAudio.log，把"游戏正在播放的 wem"变成条目触发。
// 只在 gEnabled 开启时调用（WorkerProc 内，与 FSM 判定同线程、同锁规则）。
void PollWemEvents(std::mt19937& rng)
{
    const std::uint64_t now = ::GetTickCount64();
    if (now - gWemLastPollMs < 100) return;
    gWemLastPollMs = now;
    if (gWemLogPath.empty()) return;

    // 文件大小：重建/截断检测。log 被清空重写时绝不要重放旧事件。
    std::uint64_t sz = 0;
    {
        HANDLE h = ::CreateFileW(gWemLogPath.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            if (::GetFileSizeEx(h, &li) && li.QuadPart > 0) sz = (std::uint64_t)li.QuadPart;
            ::CloseHandle(h);
        }
    }
    if (sz == 0) return;
    if (gWemStream.is_open() && sz < gWemLastSize) {
        // 文件被重建/截断 → 丢掉旧内容，只从新文件末尾开始（不读中间的历史行）
        gWemStream.close();
        gWemStream.open(gWemLogPath, std::ios::binary);
        if (gWemStream.is_open()) gWemStream.seekg(0, std::ios::end);
        gWemLastSize = sz;
        return;
    }
    if (!gWemStream.is_open()) {
        gWemStream.open(gWemLogPath, std::ios::binary);
        if (!gWemStream.is_open()) return;
        gWemStream.seekg(0, std::ios::end);   // ★ 首次打开只读新内容，绝不重放历史
        gWemLastSize = sz;
        return;
    }
    if (gWemStream.eof()) gWemStream.clear();

    struct Hit { long long media; int weapon; };
    std::vector<Hit> hits;
    std::string line;
    while (std::getline(gWemStream, line)) {
        if (line.size() < 8) continue;
        // ★ media= 才是真 media id；wem= 是 wem 字节大小（同 size 无法区分，不能当键）
        const std::size_t mp = line.find("media=");
        if (mp == std::string::npos) continue;
        const long long m = std::strtoll(line.c_str() + mp + 6, nullptr, 10);
        if (m <= 0) continue;
        std::string bank;
        const std::size_t bp = line.find("bank=");
        if (bp != std::string::npos) {
            std::size_t en = bp + 5;
            while (en < line.size() && line[en] != ' ' && line[en] != '\t' && line[en] != '\r') ++en;
            bank = line.substr(bp + 5, en - (bp + 5));
        }
        hits.push_back({ m, WemWeaponFromBank(bank) });
    }
    gWemStream.clear();
    gWemStream.seekg(0, std::ios::end);
    gWemLastSize = sz;
    if (hits.empty()) return;

    std::lock_guard<std::mutex> lk(gCfgMutex);
    for (const auto& h : hits) {
        if (h.media == gWemLastId && now - gWemLastPlayMs < 300)
            continue;   // 同一条 wem 300ms 内只响一次
        for (auto& e : gAttacks) {
            if (e.media != h.media) continue;
            if (!WeaponPermitted(e.weaponType, h.weapon)) continue;   // 武器触发开关（默认关）
            if (e.weaponType >= 0 && h.weapon >= 0 && e.weaponType != h.weapon)
                continue;
            if (e.stop) {                       // Stop 条目：命中即停（不播放）
                audio::StopAll();
                gWemLastId = h.media;
                gWemLastPlayMs = now;
                LogD("[wem] stop %s (media=%lld)", e.name.c_str(), (long long)h.media);
                break;
            }
            if (now - e.lastTriggerMs < (std::uint64_t)gDebounceMs) break;   // 每条目独立防抖
            const std::string tag = "a[" + e.name + "] media=" + std::to_string((long long)h.media);
            if (FireEntry(e, player::gGauge, rng, now, tag)) {
                e.lastTriggerMs = now;
                gWemLastId = h.media;
                gWemLastPlayMs = now;
                LogD("[wem] fired %s (bank wp? weapon=%d)", e.name.c_str(), h.weapon);
            }
            break;
        }
    }
}

std::vector<Attack> SnapshotAttacks()
{
    std::lock_guard<std::mutex> lk(gCfgMutex);
    return gAttacks;
}

// --- wav cache: guarded by gCacheMutex, populated by preload ---
std::mutex gCacheMutex;
std::vector<std::pair<std::string, audio::Wav>> gCache;

// Per-file replay cooldown (random AND fixed layer), ms: stops the same sound
// file from stacking on itself across duplicate trigger instants/entries.
// 每个音效文件的运行状态：上次触发时间 + 预计播放结束时间（用于 C<ms> 冷却 / P 播放锁）
struct SoundFileRt { std::string key; std::uint64_t lastMs = 0; std::uint64_t playingUntilMs = 0; };
std::vector<SoundFileRt> soundCooldown;
const int kSoundCooldownMs = 400;

// Action-group state: entries sharing one Group= name belong to a single
// logical attack; the group fires at most once per action occurrence, with the
// gauge level frozen at the first trigger instant (mid-action level-ups like
// the LS spirit finisher no longer fire the attack twice).
struct GroupRt { std::uint64_t lastSeen = 0; bool fired = false; std::uint64_t lastTriggerMs = 0; };
std::vector<std::pair<std::string, GroupRt>> g_groups;
const std::uint64_t kGroupResetMs = 450;   // quiet time before the group re-arms

GroupRt& GroupState(const std::string& g)
{
    for (auto& p : g_groups)
        if (p.first == g) return p.second;
    g_groups.emplace_back(g, GroupRt());
    return g_groups.back().second;
}

// ---- per-weapon config combos ----
// 每个武器可有多个命名"组合"(combo)，每个组合是一组该武器的 [Attack] 条目；
// [Active] 表决定每武器当前用哪个组合。切换某武器组合只影响该武器，其它武器不动。
std::map<int, std::map<std::string, std::vector<Attack>>> g_combos;  // weapon -> comboName -> attacks
std::map<int, std::string>  g_activeCombo;                            // weapon -> active comboName (""=默认)
std::map<int, std::vector<std::string>> g_comboOrder;                 // weapon -> combo names (默认""在前)

// 依据 g_combos + g_activeCombo 重建"当前激活"的攻击条目表（运行线程只遍历 gAttacks）。
//
// 注意：本函数分两层，防止**重复加锁**（std::mutex 非递归，同线程二次 lock 是未定义行为，
// MSVC 下会抛 system_error 直接把进程带崩——Ctrl+F11 切组合曾经就是这样闪退的）：
//   * RebuildActiveAttacksLocked() —— 调用方必须已持有 gCfgMutex
//   * RebuildActiveAttacks()       —— 自行加锁，给不持锁的调用方用
void RebuildActiveAttacksLocked()
{
    std::vector<Attack> acts;
    for (const auto& wc : g_combos) {
        const int w = wc.first;
        if (w == -1) {   // 任意武器：仅其"默认"组合常驻
            auto it = wc.second.find("");
            if (it != wc.second.end()) acts.insert(acts.end(), it->second.begin(), it->second.end());
            continue;
        }
        std::string active = "";
        auto a = g_activeCombo.find(w);
        if (a != g_activeCombo.end()) active = a->second;
        if (!wc.second.count(active)) active = "";   // 选中的组合不存在 → 回退默认
        auto it = wc.second.find(active);
        if (it != wc.second.end()) acts.insert(acts.end(), it->second.begin(), it->second.end());
    }
    gAttacks = std::move(acts);
    g_groups.clear();   // 组合切换后旧组状态作废
}

void RebuildActiveAttacks()
{
    std::lock_guard<std::mutex> lk(gCfgMutex);
    RebuildActiveAttacksLocked();
}

std::uint64_t gLastTrigger = 0;

// ===========================================================================
//  Text helpers
// ===========================================================================

inline std::string Trim(const std::string& s)
{
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string ToLower(std::string s)
{
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

// Split on ';' ',' (values may be UTF-8)
void SplitList(const std::string& value, std::vector<std::string>& out)
{
    std::string cur;
    for (std::size_t i = 0; i <= value.size(); ++i) {
        char cc = (i < value.size()) ? value[i] : '\0';
        if (cc == '\0' || cc == ';' || cc == ',') {
            std::string tok = Trim(cur);
            if (!tok.empty()) out.push_back(tok);
            cur.clear();
            if (cc == '\0') break;
        } else {
            cur += cc;
        }
    }
}

// Split one sound token on '|':  path [ | delay | vol | flags ]
// flags may contain 'F' -> fixed.
void ParseSoundSpec(const std::string& token, SoundSpec& sp)
{
    sp = SoundSpec{};
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i <= token.size(); ++i) {
        char cc = (i < token.size()) ? token[i] : '\0';
        if (cc == '\0' || cc == '|') {
            parts.push_back(Trim(cur));
            cur.clear();
            if (cc == '\0') break;
        } else {
            cur += cc;
        }
    }

    sp.path = parts.empty() ? "" : parts[0];
    if (parts.size() >= 4) {
        // explicit form: path|delay|vol|flags
        sp.plain = false;
        sp.delay = parts[1].empty() ? 0 : std::atoi(parts[1].c_str());
        if (sp.delay < 0) sp.delay = 0;
        if (sp.delay > 5000) sp.delay = 5000;
        int v = parts[2].empty() ? 100 : std::atoi(parts[2].c_str());
        sp.vol = v < 0 ? 0 : (v > 100 ? 100 : v);
        std::string flags = ToLower(parts[3]);
        sp.fixed = flags.find('f') != std::string::npos;
        sp.playLock = flags.find('p') != std::string::npos;
        {
            const std::size_t cp = flags.find('c');
            if (cp != std::string::npos) {
                int n = 0; std::size_t i = cp + 1;
                while (i < flags.size() && flags[i] >= '0' && flags[i] <= '9') { n = n * 10 + (flags[i] - '0'); ++i; }
                if (n < 0) n = 0;
                if (n > 60000) n = 60000;
                sp.cdMs = n;
            }
        }
    }
    // parts.size() == 2 / 3 without flags: accept path|delay|vol too
    else if (parts.size() == 3) {
        sp.plain = false;
        sp.delay = parts[1].empty() ? 0 : std::atoi(parts[1].c_str());
        if (sp.delay < 0) sp.delay = 0;
        if (sp.delay > 5000) sp.delay = 5000;
        int v = parts[2].empty() ? 100 : std::atoi(parts[2].c_str());
        sp.vol = v < 0 ? 0 : (v > 100 ? 100 : v);
    } else if (parts.size() == 2) {
        sp.plain = false;
        sp.delay = parts[1].empty() ? 0 : std::atoi(parts[1].c_str());
        if (sp.delay < 0) sp.delay = 0;
        if (sp.delay > 5000) sp.delay = 5000;
    }
}

// Read a whole ini file as UTF-8, skipping a BOM if present.
bool ReadFileUtf8(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(LongPathW(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (16LL << 20)) {
        ::CloseHandle(h);
        return false;
    }
    out.resize(static_cast<std::size_t>(sz.QuadPart));
    DWORD rd = 0;
    BOOL ok = ::ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    out.resize(rd);
    if (out.size() >= 3 &&
        (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

// 覆盖写整个文件（宽路径）
bool WriteFileBytes(const std::wstring& path, const std::string& data)
{
    HANDLE h = ::CreateFileW(LongPathW(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    if (!data.empty()) {
        DWORD wr = 0;
        ok = ::WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr) &&
             wr == (DWORD)data.size();
    }
    ::CloseHandle(h);
    return ok;
}

// ===========================================================================
//  组合切换（热键 / 聊天框共用）
//  切完把选择写回 ini 的 [Active] 段 —— 否则下次 Ctrl+F5 重载、重开游戏又会
//  回到旧组合，用户看起来就是“切了没用”。
// ===========================================================================
std::uint32_t gPersistActiveCombo = 1;   // ini PersistActiveCombo=0 可关掉写回

std::string ComboCurrent(int w)
{
    auto a = g_activeCombo.find(w);
    return (a != g_activeCombo.end()) ? a->second : std::string();
}

std::vector<std::string> ComboList(int w)
{
    auto it = g_comboOrder.find(w);
    return (it != g_comboOrder.end()) ? it->second : std::vector<std::string>();
}

std::string ComboDisplayName(const std::string& name) { return name.empty() ? "默认" : name; }

// 只改 ini 里 [Active] 段的 W<w>=<名字>，其它内容原样保留
bool PersistActiveCombo(int w, const std::string& name)
{
    if (gPersistActiveCombo == 0) return false;
    std::string txt;
    if (!ReadFileUtf8(gIniPath, txt) || txt.empty()) {
        Log("combo: 写回失败（读不到 ini）");
        return false;
    }
    const std::string eol = (txt.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
    std::vector<std::string> lines;
    {
        std::string cur;
        for (std::size_t i = 0; i < txt.size(); ++i) {
            if (txt[i] == '\n') {
                if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                lines.push_back(cur);
                cur.clear();
            } else {
                cur += txt[i];
            }
        }
        if (!cur.empty()) { if (cur.back() == '\r') cur.pop_back(); lines.push_back(cur); }
    }

    const std::string want  = "W" + std::to_string(w) + "=" + name;
    const std::string wantK = "w" + std::to_string(w);
    int secStart = -1;
    int secEnd   = (int)lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string t = Trim(lines[i]);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            const std::string sec = Trim(t.substr(1, t.size() - 2));
            if (sec == "Active") { secStart = (int)i; continue; }
            if (secStart >= 0) { secEnd = (int)i; break; }
        }
    }

    bool replaced = false;
    if (secStart >= 0) {
        for (int i = secStart + 1; i < secEnd; ++i) {
            const std::string t = Trim(lines[i]);
            if (t.size() > 1 && (t[0] == 'W' || t[0] == 'w')) {
                const std::size_t eq = t.find('=');
                const std::string k = ToLower((eq == std::string::npos) ? t : t.substr(0, eq));
                if (k == wantK) { lines[i] = want; replaced = true; break; }
            }
        }
        if (!replaced) lines.insert(lines.begin() + secEnd, want);
    } else {
        int at = (int)lines.size();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string t = Trim(lines[i]);
            if (t.size() > 7 && t[0] == '[' && t.compare(1, 6, "Attack") == 0) { at = (int)i; break; }
        }
        std::vector<std::string> add;
        add.push_back(std::string());
        add.push_back("[Active]");
        add.push_back(want);
        add.push_back(std::string());
        lines.insert(lines.begin() + at, add.begin(), add.end());
    }

    std::string out;
    out.reserve(txt.size() + 32);
    for (const auto& l : lines) { out += l; out += eol; }

    const std::wstring tmp = gIniPath + L".tmp";
    if (!WriteFileBytes(tmp, out) ||
        !::MoveFileExW(LongPathW(tmp).c_str(), LongPathW(gIniPath).c_str(),
                       MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(LongPathW(tmp).c_str());
        Log("combo: 写回 ini 失败（目录只读？）");
        return false;
    }
    Log("combo: [Active] W%d=%s 已写回 ini", w, name.c_str());
    return true;
}

// 切到指定组合（name="" 表示默认组合）。err 里带失败原因，便于回显。
bool ComboSet(int w, const std::string& name, std::string& err)
{
    if (w < 0) { err = "未进入场景（读不到当前武器）"; return false; }
    {
        std::lock_guard<std::mutex> lk(gCfgMutex);
        const std::vector<std::string> order = ComboList(w);
        if (order.empty()) { err = "该武器没有任何动作条目"; return false; }
        bool found = false;
        for (const auto& c : order) if (c == name) { found = true; break; }
        if (!found) {
            err = "没有叫「" + ComboDisplayName(name) + "」的组合；可用：";
            for (std::size_t i = 0; i < order.size(); ++i)
                err += (i ? " / " : "") + ComboDisplayName(order[i]);
            return false;
        }
        g_activeCombo[w] = name;
        RebuildActiveAttacksLocked();
    }
    PersistActiveCombo(w, name);
    return true;
}

// 切到下一个组合。调用方负责 PersistActiveCombo（避免在锁内做文件 I/O）。
bool ComboNext(int w, std::string& next, std::string& err)
{
    if (w < 0) { err = "未进入场景（读不到当前武器）"; return false; }
    std::lock_guard<std::mutex> lk(gCfgMutex);
    const std::vector<std::string> order = ComboList(w);
    if (order.empty()) { err = "该武器没有任何动作条目"; return false; }
    const std::string cur = ComboCurrent(w);
    std::string nxt = order[0];
    for (std::size_t k = 0; k < order.size(); ++k)
        if (order[k] == cur) { nxt = order[(k + 1) % order.size()]; break; }
    g_activeCombo[w] = nxt;
    RebuildActiveAttacksLocked();
    next = nxt;
    return true;
}

std::uintptr_t ParseHex(const std::string& s0)
{
    std::string s = Trim(s0);
    const char* p = s.c_str();
    while (*p == ' ' || *p == '\t') ++p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    unsigned long long hv = 0;
    if (sscanf_s(p, "%llx", &hv) != 1) return 0;
    return static_cast<std::uintptr_t>(hv);
}

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ===========================================================================
//  Config loading
// ===========================================================================

// Append parsed specs to a pool. plainOrder records (pool, index) of plain
// legacy tokens so SoundDelay/SoundVol can be applied later without dangling
// pointers across vector growth.
void AppendSpecs(Pool& pool, const std::string& value,
                 std::vector<std::pair<Pool*, int>>& plainOrder)
{
    std::vector<std::string> toks;
    SplitList(value, toks);
    for (const auto& tok : toks) {
        if (tok.empty()) continue;
        SoundSpec sp;
        ParseSoundSpec(tok, sp);
        if (sp.path.empty()) continue;
        // dedupe by path within the pool (F and random sets stay disjoint)
        bool dup = false;
        for (const auto& ex : pool.specs) {
            if (ex.path == sp.path) { dup = true; break; }
        }
        if (dup) {
            LogD("config: duplicate sound '%s' ignored", sp.path.c_str());
            continue;
        }
        const int idx = (int)pool.specs.size();
        const bool wasPlain = sp.plain;
        pool.specs.push_back(std::move(sp));
        if (wasPlain) plainOrder.emplace_back(&pool, idx);
    }
}

std::wstring AbsFor(const std::string& rel);

void LoadConfig()
{
    // drop decoded wavs so edited files are re-read from disk
    { std::lock_guard<std::mutex> lk(gCacheMutex); gCache.clear(); }

    std::string txt;
    if (!ReadFileUtf8(gIniPath, txt) || txt.empty()) {
        player::gRoot = gPlayerRoot;
        {
            std::lock_guard<std::mutex> lk(gCfgMutex);
            gAttacks.clear();
            g_groups.clear();
        }
        g_combos.clear();
        g_activeCombo.clear();
        g_comboOrder.clear();
        Log("no config file; using defaults (PlayerRoot=0x%llX)",
            (unsigned long long)gPlayerRoot);
        return;
    }

    // ---- combo-aware parse ----
    // key 形式： "cb|<weapon>|<comboName>" 或 "def|<weapon>"(无组合的旧条目/默认组合)
    std::map<std::string, std::vector<Attack>> comboData;
    std::map<int, std::string> activeMap;                 // [Active] W<type>=<combo>
    bool inAttack = false, inCombo = false;
    int  curComboW = -1;
    std::string section, curComboName;
    Attack cur;
    std::vector<int> rawDelay, rawVol;
    std::vector<std::pair<Pool*, int>> plainOrder;

    auto finishEntry = [&]() {
        if (!inAttack) return;
        for (std::size_t i = 0; i < plainOrder.size(); ++i) {
            SoundSpec& sp = plainOrder[i].first->specs[plainOrder[i].second];
            if (i < rawDelay.size() && rawDelay[i] > 0) sp.delay = rawDelay[i];
            if (i < rawVol.size()) sp.vol = ClampInt(rawVol[i], 0, 100);
        }
        // 旧版本 GUI 保存默认组合的条目时没写 [WeaponW] 段头，条目会掉进上一个命名
        // 组合的段里（长枪条目变成"武器3的组合DIO"）。段里的组合武器和条目自己的
        // WeaponType 不一致 → 判为错位，按该武器自己的默认组合处理。
        // （WeaponType=-1 的任意武器条目同理：组合只对具体武器有意义）
        const bool misplaced = inCombo && curComboW >= 0 && cur.weaponType != curComboW;
        if (misplaced)
            LogD("config: 条目 \"%s\" 段头是武器%d的组合[%s]但自己是武器%d，按默认组合处理",
                 cur.name.c_str(), curComboW, curComboName.c_str(), cur.weaponType);
        std::string key = (inCombo && curComboW >= 0 && !misplaced)
                              ? ("cb|" + std::to_string(curComboW) + "|" + curComboName)
                              : ("def|" + std::to_string(cur.weaponType));
        comboData[key].push_back(std::move(cur));
        inAttack = false;
        cur = Attack();
        rawDelay.clear();
        rawVol.clear();
        plainOrder.clear();
    };

    std::size_t pos = 0;
    while (pos < txt.size()) {
        std::size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = txt.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[' && line.back() == ']') {
            finishEntry();
            section = Trim(line.substr(1, line.size() - 2));
            if (section.size() >= 6 && section.compare(0, 6, "Attack") == 0) {
                inAttack = true;          // 组合上下文(若有)延续
                cur = Attack();
            } else if (section == "Active") {
                inAttack = false; inCombo = false; curComboW = -1; curComboName.clear();
            } else if (section == "Combos") {
                // [Combos] W<type>=组合1;组合2 —— GUI 持久化的组合名列表
                // （空组合也在这）。这里只重置上下文，真正解析在值处理处。
                inAttack = false; inCombo = false; curComboW = -1; curComboName.clear();
            } else if (section.size() > 6 && section.compare(0, 6, "Weapon") == 0) {
                // [Weapon<type>:<comboName>]
                std::string rest = section.substr(6);
                std::size_t c = rest.find(':');
                if (c != std::string::npos) {
                    curComboW = std::atoi(rest.substr(0, c).c_str());
                    curComboName = rest.substr(c + 1);
                    inCombo = true; inAttack = false;
                } else {
                    inCombo = false; curComboW = -1; curComboName.clear(); inAttack = false;
                }
            } else {
                // 其它段：重置组合上下文
                inCombo = false; curComboW = -1; curComboName.clear(); inAttack = false;
            }
            continue;
        }

        // 找键值分隔用的 '='。不能直接用第一个 —— 条件表达式里的比较符
        // (>= <= == !=) 也含 '='，比如
        //     Sound:dmg>0 & dAura>=0 = sounds/a.wav
        // 按第一个 '=' 切会把键截成 "Sound:dmg>0 & dAura>"，表达式就废了。
        // 规则：跳过属于比较符的 '=' —— 前一个字符是 > < ! = ，或后一个字符是 = 。
        std::size_t eq = std::string::npos;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] != '=') continue;
            const char prev = (i > 0) ? line[i - 1] : (char)0;
            const char next = (i + 1 < line.size()) ? line[i + 1] : (char)0;
            if (prev == '>' || prev == '<' || prev == '!' || prev == '=') continue;
            if (next == '=') continue;
            eq = i; break;
        }
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));

        if (!inAttack) {
            if (section == "WeaponSoundEnhance") {
                if (key == "PlayerRoot")  gPlayerRoot = ParseHex(val);
                else if (key == "PollMs") { int v = std::atoi(val.c_str()); if (v >= 5) gPollMs = v; }
                else if (key == "DebounceMs") { int v = std::atoi(val.c_str()); if (v >= 0) gDebounceMs = v; }
                else if (key == "KillSettleMs") { int v = std::atoi(val.c_str()); if (v >= 0 && v <= 60000) gKillSettleMs = v; }
                else if (key == "WemBurstCap") { int v = std::atoi(val.c_str()); if (v >= 0 && v <= 1000) gWemBurstCap = v; }
                else if (key == "WemLoopFilter") gWemLoopFilter = std::atoi(val.c_str()) != 0;
                else if (key == "WemLoopRate") { int v = std::atoi(val.c_str()); if (v >= 1 && v <= 100) gWemLoopRate = v; }
                else if (key == "WemLoopMuteMs") { int v = std::atoi(val.c_str()); if (v >= 0 && v <= 120000) gWemLoopMuteMs = v; }
                else if (key == "WeaponEnabled") {
                    // 14 个逗号分隔的 0/1（武器 0..13），缺省按 0（关）
                    std::vector<std::string> toks;
                    SplitList(val, toks);
                    for (int i = 0; i < 14; ++i)
                        gWeaponEnabled[i] = (i < (int)toks.size() && std::atoi(toks[i].c_str()) != 0);
                }
                else if (key == "WemOnlyInQuest") gWemOnlyInQuest = std::atoi(val.c_str()) != 0;
                else if (key == "WemMapWhite") {
                    gWemMapWhite.clear();
                    std::vector<std::string> toks; SplitList(val, toks);
                    for (const auto& t : toks) gWemMapWhite.push_back(std::atoi(t.c_str()));
                }
                else if (key == "WemMapBlack") {
                    gWemMapBlack.clear();
                    std::vector<std::string> toks; SplitList(val, toks);
                    for (const auto& t : toks) gWemMapBlack.push_back(std::atoi(t.c_str()));
                }
                else if (key == "WemIgnoreMedia") {
                    gWemIgnoreMedia.clear();
                    std::vector<std::string> toks;
                    SplitList(val, toks);
                    for (const auto& t : toks) {
                        const long long v = std::strtoll(t.c_str(), nullptr, 10);
                        if (v > 0) gWemIgnoreMedia.push_back(v);
                    }
                }
                else if (key == "Volume") gVolumePct = ClampInt(std::atoi(val.c_str()), 0, 100);
                else if (key == "Enabled") gEnabled = std::atoi(val.c_str()) != 0;
                else if (key == "MoreSounds") gMoreSounds = std::atoi(val.c_str()) != 0;
            else if (key == "EfxCapture") gEfxCapture = std::atoi(val.c_str()) != 0;
            else if (key == "EfxLogAll") gEfxLogAll = std::atoi(val.c_str()) != 0;
            else if (key == "EfxDiscover") gEfxDiscover = std::atoi(val.c_str()) != 0;
            else if (key == "EfxTablePref") gEfxTablePref = val;
                else if (key == "ChatEcho") g_useChatEcho = std::atoi(val.c_str()) != 0;
                else if (key == "ChatCommands") g_useChatCommands = std::atoi(val.c_str()) != 0;
                else if (key == "Hotkeys") g_hotkeysEnabled = std::atoi(val.c_str()) != 0;
                else if (key == "PersistActiveCombo") gPersistActiveCombo = std::atoi(val.c_str()) != 0;
                else if (key == "WatchMode") gWatchMode = ClampInt(std::atoi(val.c_str()), 0, 3);
                else if (key == "LayerProbe") gLayerProbeEnabled = std::atoi(val.c_str()) != 0;
                else if (key == "Debug") gDebug = std::atoi(val.c_str()) != 0;
                else if (key == "GaugePtrOff") { std::uintptr_t v = ParseHex(val); if (v) gGaugePtrOff = (std::uint32_t)v; }
                else if (key == "GaugeValOff") { std::uintptr_t v = ParseHex(val); if (v) gGaugeValOff = (std::uint32_t)v; }
                else if (key == "ChargeValOff") gChargeValOff = (std::uint32_t)ParseHex(val);
                else if (key == "FsmTargetOff") gFsmTargetOff = (std::uint32_t)ParseHex(val);
                else if (key == "QuestRoot")    gQuestRoot    = ParseHex(val);
                else if (key == "QuestDmgOff")  gQuestDmgOff  = (std::uint32_t)ParseHex(val);
                else if (key == "HitMonsterOff")   gHitMonOff   = (std::uint32_t)ParseHex(val);
                else if (key == "MonHealthPtrOff") gMonHpPtrOff = (std::uint32_t)ParseHex(val);
                else if (key == "MonHealthMaxOff") gMonHpMaxOff = (std::uint32_t)ParseHex(val);
                else if (key == "MonHealthCurOff") gMonHpCurOff = (std::uint32_t)ParseHex(val);
            } else if (section == "Hotkeys") {
                if (key == "ModifierKey") gModifierKey = std::atoi(val.c_str());
                else if (key == "ReloadKey")   gReloadKey   = std::atoi(val.c_str());
                else if (key == "VolUpKey")    gVolUpKey    = std::atoi(val.c_str());
                else if (key == "VolDownKey")  gVolDownKey  = std::atoi(val.c_str());
                else if (key == "SetVolKey")   gSetVolKey   = std::atoi(val.c_str());
                else if (key == "ToggleKey")   gToggleKey   = std::atoi(val.c_str());
                else if (key == "MoreKey")     gMoreKey     = std::atoi(val.c_str());
                else if (key == "SetVolValue") gSetVolValue = ClampInt(std::atoi(val.c_str()), 0, 100);
                else if (key == "ComboKey")    gComboKey    = std::atoi(val.c_str());
            } else if (section == "Combos") {
                // W<type>=组合1;组合2 —— 注册组合名（空组合也生效），
                // 这样游戏内 Ctrl+F11 循环和 /wse combo 能看到还没有条目的组合。
                if (key.size() > 1 && (key[0] == 'W' || key[0] == 'w')) {
                    int w = std::atoi(key.c_str() + 1);
                    if (w < 0 || w > 13) continue;
                    std::string cur;
                    for (std::size_t i = 0; i <= val.size(); ++i) {
                        if (i == val.size() || val[i] == ';' || val[i] == ',') {
                            std::string nm = Trim(cur);
                            cur.clear();
                            if (nm.empty()) continue;
                            const std::string k = "cb|" + std::to_string(w) + "|" + nm;
                            if (!comboData.count(k)) comboData[k] = std::vector<Attack>();  // 空组合
                        } else {
                            cur += val[i];
                        }
                    }
                }
            } else if (section == "Active") {
                // W<type>=<comboName>
                if (key.size() > 1 && (key[0] == 'W' || key[0] == 'w')) {
                    int w = std::atoi(key.c_str() + 1);
                    if (w >= -1) activeMap[w] = val;
                }
            }
            continue;
        }

        // ---- attack section keys ----
        if (key == "WeaponType") {
            cur.weaponType = std::atoi(val.c_str());
        } else if (key == "FSMId") {
            cur.fsmId = std::atoi(val.c_str());
        } else if (key == "Media") {
            cur.media = std::strtoll(val.c_str(), nullptr, 10);
        } else if (key == "FSMTarget") {
            cur.fsmTarget = std::atoi(val.c_str());
        } else if (key == "ActionLMT" || key == "LMT") {
            std::vector<std::string> toks;
            SplitList(val, toks);          // ';' ',' 分隔
            for (const auto& t0 : toks) {
                // 再按空格切一次，容忍 "LMT=49265 49256" 这种写法
                std::string curTok;
                std::vector<std::string> parts;
                for (std::size_t i2 = 0; i2 <= t0.size(); ++i2) {
                    const char cc = (i2 < t0.size()) ? t0[i2] : '\0';
                    if (cc == '\0' || cc == ' ' || cc == '\t') {
                        const std::string p = Trim(curTok);
                        if (!p.empty()) parts.push_back(p);
                        curTok.clear();
                        if (cc == '\0') break;
                    } else {
                        curTok += cc;
                    }
                }
                for (const auto& t : parts) {
                    const std::string low = ToLower(Trim(t));
                    // 不限：-1 / any / all / * / 不限 → 清空列表（空列表 = 任意 LMT）
                    if (low == "-1" || low == "any" || low == "all" || low == "*" || low == "不限") {
                        cur.lmt.clear();
                        break;
                    }
                    // 严格整数：以前用 atoi，"49265-1" 会被静默截断成 49265，
                    // 用户以为已经改成不限了，实际配置没变 —— 这类项直接忽略并记日志。
                    bool digits = !t.empty();
                    const std::size_t st = (t[0] == '+' || t[0] == '-') ? 1 : 0;
                    if (st >= t.size()) digits = false;
                    for (std::size_t i3 = st; i3 < t.size() && digits; ++i3)
                        if (t[i3] < '0' || t[i3] > '9') digits = false;
                    if (!digits) {
                        LogD("config: LMT 项 \"%s\" 不是整数，已忽略（空或 -1 = 不限）", t.c_str());
                        continue;
                    }
                    const int v = std::atoi(t.c_str());
                    if (v < 0) continue;
                    bool dup = false;
                    for (int x : cur.lmt) if (x == v) { dup = true; break; }
                    if (!dup) cur.lmt.push_back(v);
                }
            }
        } else if (key == "Name") {
            cur.name = val;
        } else if (key == "Stop") {
            cur.stop = std::atoi(val.c_str()) != 0;   // Stop=1：命中即停止播放
        } else if (key == "Group") {
            cur.group = val;
        } else if (key == "CheckDelayMs") {
            cur.checkDelayMs = std::atoi(val.c_str());
        } else if (key == "CheckTimeoutMs") {
            cur.checkTimeoutMs = std::atoi(val.c_str());
        } else if (key == "CheckMode") {
            cur.checkMode = (ToLower(val) == "final") ? 1 : 0;
        } else if (key == "CheckEndOn") {
            cur.checkEndOn = (ToLower(val) == "action") ? 1 : 0;
        } else if (key == "CheckGraceMs" || key == "CheckOffsetMs") {
            const int off = std::atoi(val.c_str());
            if (off < 0) {
                LogD("config: %s=%d 负值已不再支持，按 0 处理（见 checkGraceMs 的注释）",
                     key.c_str(), off);
                cur.checkGraceMs = 0;
            } else {
                cur.checkGraceMs = off;
            }
        } else if (key == "Sound") {
            AppendSpecs(cur.defPool, val, plainOrder);
        } else if ((key.size() > 6 && key.compare(0, 6, "Sound:") == 0) ||
                   (key.size() > 9 && key.compare(0, 9, "SoundEnd:") == 0)) {
            const bool atEnd = (key.size() > 9 && key.compare(0, 9, "SoundEnd:") == 0);
            const std::string tag = key.substr(atEnd ? 9 : 6);
            cond::Expr ex;
            if (cond::LooksLikeExpr(tag) && cond::Parse(tag, ex)) {
                // 条件池。注意这里**不登记进 plainOrder** —— plainOrder 存的是
                // Pool* ，而 conds 是 vector，push_back 扩容会让指针失效。
                // 代价是旧式的 SoundDelay=/SoundVol= 对条件池不生效，
                // 用新式的 路径|延时|音量|F 内联写法即可。
                CondPool cp;
                cp.text  = cond::Trim(tag);
                cp.expr  = ex;
                cp.atEnd = atEnd;
                std::vector<std::pair<Pool*, int>> scratch;
                AppendSpecs(cp.pool, val, scratch);
                cur.conds.push_back(std::move(cp));
            } else {
                const int lvl = GaugeTagToLevel(ToLower(tag));
                if (lvl >= 0 && lvl < 4)
                    AppendSpecs(cur.gaugePool[lvl], val, plainOrder);
                else
                    LogD("config: unknown gauge tag / bad expression '%s' ignored", key.c_str());
            }
        } else if (key == "SoundDelay") {
            std::vector<std::string> toks;
            SplitList(val, toks);
            for (const auto& t : toks)
                rawDelay.push_back(t.empty() ? 0 : std::atoi(t.c_str()));
        } else if (key == "SoundVol") {
            std::vector<std::string> toks;
            SplitList(val, toks);
            for (const auto& t : toks)
                rawVol.push_back(t.empty() ? 100 : std::atoi(t.c_str()));
        }
    }
    finishEntry();

    player::gRoot = gPlayerRoot;
    player::gGaugePtrOff = gGaugePtrOff;
    player::gGaugeValOff = gGaugeValOff;
    player::gChargeValOff = gChargeValOff;
    player::gFsmTargetOff = gFsmTargetOff;
    player::gQuestRoot    = gQuestRoot;
    player::gQuestDmgOff  = gQuestDmgOff;
    player::gHitMonOff    = gHitMonOff;
    player::gMonHpPtrOff  = gMonHpPtrOff;
    player::gMonHpMaxOff  = gMonHpMaxOff;
    player::gMonHpCurOff  = gMonHpCurOff;

    // 组装 per-weapon combos 结构（按出现顺序记 order）
    g_combos.clear();
    g_comboOrder.clear();
    for (const auto& kv : comboData) {
        const std::string& k = kv.first;
        int w = -1; std::string cname = "";
        if (k.compare(0, 4, "def|") == 0) {
            w = std::atoi(k.c_str() + 4);
            cname = "";
        } else if (k.compare(0, 3, "cb|") == 0) {
            std::string rest = k.substr(3);
            std::size_t p = rest.find('|');
            if (p != std::string::npos) {
                w = std::atoi(rest.substr(0, p).c_str());
                cname = rest.substr(p + 1);
            }
        }
        auto& vec = g_combos[w][cname];
        vec.insert(vec.end(), kv.second.begin(), kv.second.end());
        auto& order = g_comboOrder[w];
        bool has = false;
        for (const auto& o : order) if (o == cname) { has = true; break; }
        if (!has) order.push_back(cname);
    }
    // 默认组合 "" 放最前（保证热键从默认开始）
    for (auto& wo : g_comboOrder) {
        auto& order = wo.second;
        bool hasDef = false;
        for (const auto& o : order) if (o.empty()) { hasDef = true; break; }
        if (hasDef) {
            std::vector<std::string> sorted;
            sorted.push_back("");
            for (const auto& o : order) if (!o.empty()) sorted.push_back(o);
            order.swap(sorted);
        }
    }
    // [Active] 里指向"该武器并不存在的组合"的映射一律丢掉：历史保存 bug 会留下
    // W6=DIO 这种从别的武器串过来的名字，留着会让新条目被塞进幽灵组合。
    for (auto it = activeMap.begin(); it != activeMap.end();) {
        const int w = it->first;
        const std::string& nm = it->second;
        bool exists = nm.empty();
        if (!exists) {
            auto wc = g_combos.find(w);
            if (wc != g_combos.end() && wc->second.count(nm)) exists = true;
        }
        if (!exists) {
            LogD("config: [Active] W%d=%s 对应的组合不存在，已忽略", w, nm.c_str());
            it = activeMap.erase(it);
        } else {
            ++it;
        }
    }
    g_activeCombo = activeMap;

    RebuildActiveAttacks();
    Log("config: combos=%zu attacks=%zu", g_combos.size(), gAttacks.size());
}

void PreloadSounds();   // defined below

void ReloadConfig() {
    LoadConfig();
    PreloadSounds();
    // ★ 重载会重放/预加载 bank（用户反馈"每次重载都响一次装瓶"），
    //   所以把"稳定时刻"拨到现在：接下来 KillSettleMs 内的 wem 一律忽略。
    player::gStableSinceMs = ::GetTickCount64();
    // ★ 特效索引也跟着重建：用户改完 .epv3（换路径/加组）后点重载即可生效，
    //   不用重启游戏 —— 索引不刷新就会出现"有的特效完全捕获不到"。
    {
        std::string efxIdx;
        if (::sonarefx::ReloadIndex(efxIdx)) LogD("[efx] 索引已重建：%s", efxIdx.c_str());
        else                            LogD("[efx] 索引重建失败：%s", efxIdx.c_str());
    }
}

// ===========================================================================
//  Wav cache / preload
// ===========================================================================

// Must be called with gCacheMutex held. Loads+decodes once, then returns ptr.
const audio::Wav* GetCached(const std::wstring& absPath)
{
    const std::string key = strconv::ToUtf8(absPath);
    for (auto& p : gCache)
        if (p.first == key) return &p.second;

    // ★ 完整共享：否则我们读的时候用户的外部编辑器**无法保存**该音效文件，
    //   编辑器写到一半失败就留下半截文件（用户反馈"编辑一半音效就损坏了"）。
    HANDLE h = ::CreateFileW(LongPathW(absPath).c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("audio not found: %s", key.c_str());
        return nullptr;
    }
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64LL << 20)) {
        ::CloseHandle(h);
        Log("audio size invalid (>64MB): %s", key.c_str());
        return nullptr;
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(sz.QuadPart));
    DWORD rd = 0;
    if (!::ReadFile(h, raw.data(), (DWORD)raw.size(), &rd, nullptr) || rd != (DWORD)raw.size()) {
        ::CloseHandle(h);
        return nullptr;
    }
    ::CloseHandle(h);

    audio::Wav w;
    std::string fmtName;
    if (!audio::DecodeAudio(raw.data(), raw.size(), fmtName, w)) {
        Log("audio decode failed: %s (支持 wav/mp3/ogg/flac)", key.c_str());
        return nullptr;
    }
    // 非标准采样率（很多用户音效是 48k 之外的特殊值）才重采样到 44.1k，
    // 常见采样率原样播放，保住原始音质。
    if (!wseaudio::IsStandardRate(w.sampleRate)) {
        audio::ResampleTo(w, 44100);
        LogD("resampled: %s -> 44100 (原采样率 %u Hz)", key.c_str(), w.sampleRate);
    }
    LogD("loaded [%s]: %s | ch=%u rate=%u bits=%u bytes=%zu",
         fmtName.c_str(), key.c_str(), w.channels, w.sampleRate, w.bitsPer, w.data.size());
    gCache.emplace_back(std::move(key), std::move(w));
    return &gCache.back().second;
}

// Decode every configured wav up front so triggering never does file I/O.
void PreloadSounds()
{
    const std::vector<Attack> attacks = SnapshotAttacks();
    std::vector<std::string> absPaths;   // utf8, unique
    auto addSpecs = [&](const Pool& p) {
        for (const auto& sp : p.specs) {
            if (sp.path.empty()) continue;
            const std::string key = strconv::ToUtf8(AbsFor(sp.path));
            bool dup = false;
            for (const auto& e : absPaths) if (e == key) { dup = true; break; }
            if (!dup) absPaths.push_back(key);
        }
    };
    for (const auto& a : attacks) {
        addSpecs(a.defPool);
        for (int i = 0; i < 4; ++i) addSpecs(a.gaugePool[i]);
    }

    std::lock_guard<std::mutex> lk(gCacheMutex);
    for (const auto& p : absPaths) {
        bool cached = false;
        for (auto& e : gCache) if (e.first == p) { cached = true; break; }
        if (!cached) GetCached(strconv::ToWide(p));
    }
    Log("preload: %zu unique sound(s), cache=%zu", absPaths.size(), gCache.size());
}

// Relative "sounds/xxx.wav" -> absolute path below the module dir.
// 查找顺序：数据目录(plugins\WeaponSoundEnhance\) → 旧布局(DLL 同目录)。
// 2.3 之前 wav 都放在 plugins\sounds\ 下，这里自动兼容，用户不用搬文件；
// 已经绝对路径的(如 F:\...\a.wav)直接原样返回。
std::wstring JoinPath(const std::wstring& dir, const std::string& rel)
{
    std::wstring w = dir + strconv::ToWide(rel);
    for (auto& c : w) if (c == L'/') c = L'\\';
    std::wstring out;
    out.reserve(w.size());
    bool prevSlash = false;
    for (wchar_t c : w) {
        if (c == L'\\') {
            if (!prevSlash) out += c;
            prevSlash = true;
        } else {
            out += c;
            prevSlash = false;
        }
    }
    return out;
}

std::wstring AbsFor(const std::string& rel)
{
    // 绝对路径（盘符或 UNC）原样返回，不拼数据目录
    if (rel.size() > 1 && rel[1] == ':') return strconv::ToWide(rel);
    if (rel.size() >= 2 && (rel[0] == '\\' || rel[0] == '/') &&
        (rel[1] == '\\' || rel[1] == '/')) return strconv::ToWide(rel);

    static std::mutex pathMutex;
    static std::map<std::string, std::wstring> pathCache;
    {
        std::lock_guard<std::mutex> lk(pathMutex);
        auto it = pathCache.find(rel);
        if (it != pathCache.end()) return it->second;
    }

    std::wstring p = JoinPath(gDataDir, rel);
    if (gDataDir != gModuleDir && !FileExistsW(p)) {
        const std::wstring legacy = JoinPath(gModuleDir, rel);
        if (FileExistsW(legacy)) p = legacy;   // 旧布局回退
    }

    std::lock_guard<std::mutex> lk(pathMutex);
    pathCache[rel] = p;
    return p;
}

// ===========================================================================
//  In-game chat echo + /wse commands
// ===========================================================================

void ResolveGameBase()
{
    HMODULE mod = ::GetModuleHandleW(L"MonsterHunterWorld.exe");
    if (!mod) return;
    gGameBase = reinterpret_cast<std::uintptr_t>(mod);
    g_systemMessage = reinterpret_cast<SystemMessageFn>(gGameBase + kSystemMessageRva);
    Log("game base=0x%llX msgFn=0x%llX",
        (unsigned long long)gGameBase, (unsigned long long)g_systemMessage);
}

void ShowMessage(const char* utf8, bool emphasized = false)
{
    if (g_useChatEcho == 0) {
        Log("[echo] %s", utf8);
        return;
    }
    if (g_systemMessage == nullptr || gGameBase == 0) return;
    if (!player::RefreshIsInScene()) return;
    void* mgr = nullptr;
    if (!mem::ReadVal(gGameBase + kSystemMessageMgrRva, mgr) || mgr == nullptr) return;
    char msgbuf[0x180] = {};   // native fn copies 0x17f bytes at most
    _snprintf_s(msgbuf, _TRUNCATE, "%s", utf8);
    g_systemMessage(mgr, msgbuf, 0.0f, -1, emphasized);
}

inline bool HandleWseCommand(const std::string& rest, bool& used)
{
    used = true;
    if (rest.empty()) {
        ShowMessage("WeaponSound OK");
    } else if (rest == "reload" || rest == "re") {
        audio::StopAll();   // 重载前先停掉正在播放的音效，避免旧配置的声音继续响
        ReloadConfig();
        char msg[0x180] = {};
        _snprintf_s(msg, _TRUNCATE, "wse config reloaded (vol=%d)", (int)gVolumePct);
        ShowMessage(msg, true);
    } else if (rest == "on" || rest == "enable") {
        gEnabled = 1; ShowMessage("wse enabled", true);
    } else if (rest == "off" || rest == "disable") {
        gEnabled = 0; ShowMessage("wse disabled", true);
    } else if (rest == "more" || rest == "extra") {
        gMoreSounds = 1; ShowMessage("wse extra sounds ON", true);
    } else if (rest == "stop") {
        audio::StopAll();
        ShowMessage("wse sounds stopped", true);
    } else if (rest == "one" || rest == "single") {
        gMoreSounds = 0; ShowMessage("wse extra sounds OFF (single)", true);
    } else if (rest == "help" || rest == "h") {
        ShowMessage("/wse reload | on | off | more | one | stop | vol N | vol+ | vol- | "
                    "combo [名字] | combos (列出组合) | watch [0..3]");
    } else if (rest == "watch" || rest.rfind("watch ", 0) == 0) {
        // 监听模式：0=关 1=派生ID流 2=WEM调用流 3=两者
        // 不带参数时打印当前状态与用法。
        const std::string arg = Trim(rest.size() > 5 ? rest.substr(5) : std::string());
        char msg[0x180] = {};
        if (arg.empty()) {
            const int m = gWatchMode;
            _snprintf_s(msg, _TRUNCATE,
                "wse watch=%d (%s%s%s)", m,
                (m & 1) ? "派生ID" : "",
                (m == 3) ? "+" : "",
                (m & 2) ? "WEM调用" : "");
            if (m == 0) _snprintf_s(msg, _TRUNCATE, "wse watch=0 (关闭)");
            ShowMessage(msg, true);
            ShowMessage("用法: /wse watch 0=关 1=派生ID 2=WEM 3=两者", true);
        } else {
            const int v = ClampInt(std::atoi(arg.c_str()), 0, 3);
            gWatchMode = v;
            // 打开派生流时清一次边沿状态，保证下一帧一定会打一条当前状态
            if (v & 1) {
                gWatchLastFsm = gWatchLastFsmTgt = gWatchLastLmt = gWatchLastWeapon = -2;
            }
            _snprintf_s(msg, _TRUNCATE,
                "wse watch=%d (%s)", v,
                v == 0 ? "关闭" : (v == 1 ? "派生ID流" : (v == 2 ? "WEM调用流" : "派生ID+WEM")));
            ShowMessage(msg, true);
        }
    } else if (rest == "combo" || rest.rfind("combo ", 0) == 0 || rest.rfind("cb", 0) == 0) {
        const int w = player::gWeapon;
        std::string arg = Trim((rest.rfind("combo", 0) == 0) ? rest.substr(5) : rest.substr(2));
        std::string err;
        if (arg.empty()) {
            std::string nxt;
            if (ComboNext(w, nxt, err)) {
                PersistActiveCombo(w, nxt);
                const std::vector<std::string> order = ComboList(w);
                std::string m = "wse combo -> " + ComboDisplayName(nxt) +
                                " (" + std::to_string(order.size()) + " 个可用:";
                for (const auto& c : order) m += " " + ComboDisplayName(c);
                m += ")";
                ShowMessage(m.c_str(), true);
            }
        } else if (arg == "list" || arg == "ls" || arg == "?") {
            const std::vector<std::string> order = ComboList(w);
            if (w < 0) err = "未进入场景";
            else if (order.empty()) err = "该武器没有任何动作条目";
            else {
                const std::string cur = ComboCurrent(w);
                std::string m = "wse combos:";
                for (const auto& c : order)
                    m += (c == cur ? " [" : " ") + ComboDisplayName(c) + (c == cur ? "]" : "");
                ShowMessage(m.c_str(), true);
            }
        } else if (!ComboSet(w, arg, err)) {
            // err 已填好
        } else {
            std::string m = "wse combo -> " + ComboDisplayName(arg);
            ShowMessage(m.c_str(), true);
        }
        if (!err.empty()) {
            const std::string m = "wse combo: " + err;
            ShowMessage(m.c_str(), true);
        }
    } else if (rest == "vol+" || rest == "up") {
        gVolumePct += 5; if (gVolumePct > 100) gVolumePct = 100;
        char msg[0x180] = {}; _snprintf_s(msg, _TRUNCATE, "wse volume=%d", (int)gVolumePct);
        ShowMessage(msg, true);
    } else if (rest == "vol-" || rest == "down") {
        gVolumePct -= 5; if (gVolumePct < 0) gVolumePct = 0;
        char msg[0x180] = {}; _snprintf_s(msg, _TRUNCATE, "wse volume=%d", (int)gVolumePct);
        ShowMessage(msg, true);
    } else if (rest.rfind("vol", 0) == 0 || rest.rfind("v=", 0) == 0 ||
               (rest[0] >= '0' && rest[0] <= '9')) {
        std::string num = rest;
        if (num.rfind("vol", 0) == 0) num = num.substr(3);
        else if (num.rfind("v=", 0) == 0) num = num.substr(2);
        else if (num.rfind("v", 0) == 0) num = num.substr(1);
        num = Trim(num);
        int vv = std::atoi(num.c_str());
        if (vv >= 0 && vv <= 100) {
            gVolumePct = vv;
            char msg[0x180] = {};
            _snprintf_s(msg, _TRUNCATE, "wse volume=%d", vv);
            ShowMessage(msg, true);
        } else {
            ShowMessage("wse volume must be 0..100");
        }
    } else {
        ShowMessage("wse unknown command; try /wse help");
        used = false;
    }
    return used;
}

bool ParseCommand(const std::string& line)
{
    std::string s = Trim(line);
    if (s.empty() || s[0] != '/') return false;
    std::size_t sp = s.find_first_of(" \t");
    std::string cmd = (sp == std::string::npos) ? s.substr(1) : s.substr(1, sp - 1);
    std::string rest = (sp == std::string::npos) ? "" : Trim(s.substr(sp + 1));
    cmd = ToLower(cmd);
    if (cmd == "wse" || cmd == "wsesound" || cmd == "sound") {
        bool used = false;
        HandleWseCommand(rest, used);
        return true;
    }
    return false;
}

// Read the game's chat message buffer; /wse prefixed messages are executed and
// the buffer cleared. Pure memory reads, no function hooks.
bool PollChatCommand()
{
    if (g_useChatCommands == 0) return false;
    if (gGameBase == 0) return false;
    if (!player::RefreshIsInScene()) return false;

    std::uintptr_t base = 0;
    if (!mem::ReadVal(gGameBase + kMessageBaseRva, base) || base == 0) return false;

    std::uintptr_t lenPtr = base + kMessageLenOff;
    std::uintptr_t bodyPtr = base + kMessageBodyOff;
    if (!mem::IsReadable(lenPtr, 4) || !mem::IsReadable(bodyPtr, 4)) return false;

    std::int32_t len = 0;
    if (!mem::ReadVal(lenPtr, len) || len <= 0 || len > 1024) return false;

    std::string msg;
    if (mem::IsReadable(bodyPtr, (std::size_t)len + 1)) {
        msg.assign(reinterpret_cast<const char*>(bodyPtr), (std::size_t)len);
        while (!msg.empty() && (msg.back() == '\0' || msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
    }
    if (msg.empty()) return false;

    std::string trimmed = Trim(msg);
    if (trimmed.rfind("/wse", 0) == 0 || trimmed.rfind("/wsesound", 0) == 0) {
        static std::string lastHandled;
        static std::uint64_t lastHandledAt = 0;
        const std::uint64_t now = ::GetTickCount64();
        if (lastHandled == trimmed && (now - lastHandledAt) < 1000)
            return false;
        lastHandled = trimmed;
        lastHandledAt = now;
        LogD("[chat] '%s'", msg.c_str());
        if (mem::IsWritable(bodyPtr, (std::size_t)len + 1))
            std::memset(reinterpret_cast<void*>(bodyPtr), 0, (std::size_t)len + 1);
        if (mem::IsWritable(lenPtr, 4))
            std::memset(reinterpret_cast<void*>(lenPtr), 0, 4);
        return ParseCommand(msg);
    }
    return false;
}

// ===========================================================================
//  Trigger evaluation + playback selection
// ===========================================================================

// 取/建某个音效文件的运行状态
SoundFileRt& FileRt(const std::string& pathKey)
{
    for (auto& sc : soundCooldown)
        if (sc.key == pathKey) return sc;
    soundCooldown.push_back(SoundFileRt{pathKey, 0, 0});
    if (soundCooldown.size() > 64)
        soundCooldown.erase(soundCooldown.begin());
    return soundCooldown.back();
}

// 是否该跳过这条音效：
//  - P（playLock）：这条音效还在播放（含延时等待）时不再被重复触发
//  - C<ms>：这条音效自己的再触发冷却；没写 C 时沿用默认 400ms 的同文件冷却
bool SpecBlocked(const SoundSpec& sp, const std::string& pathKey, std::uint64_t nowMs)
{
    SoundFileRt& rt = FileRt(pathKey);
    if (rt.lastMs == 0 && rt.playingUntilMs == 0) return false;   // 第一次触发
    if (sp.playLock && nowMs < rt.playingUntilMs) return true;
    if (sp.cdMs > 0) return (nowMs - rt.lastMs) < (std::uint64_t)sp.cdMs;
    return (nowMs - rt.lastMs) < (std::uint64_t)kSoundCooldownMs;
}

// 播放成功后登记：上次触发时间 + 预计播完时间（延时 + 音频时长）
void NotePlayed(const SoundSpec& sp, const std::string& pathKey, std::uint64_t nowMs, unsigned durMs)
{
    SoundFileRt& rt = FileRt(pathKey);
    rt.lastMs = nowMs;
    const std::uint64_t end = nowMs + (std::uint64_t)sp.delay + (std::uint64_t)durMs;
    if (end > rt.playingUntilMs) rt.playingUntilMs = end;
}

// 兼容旧调用点：随机层用的「同文件冷却」
bool CooldownHit(const std::string& pathKey, std::uint64_t nowMs)
{
    SoundFileRt& rt = FileRt(pathKey);
    const bool cool = (nowMs - rt.lastMs) < (std::uint64_t)kSoundCooldownMs;
    rt.lastMs = nowMs;
    return cool;
}

// Play one spec. Caller already decided cooldown policy.
// durMsOut 返回本次音频时长（ms），用于「播放期间不重复」策略
bool PlayOne(const SoundSpec& sp, const std::string& what, unsigned* durMsOut = nullptr)
{
    if (sp.path.empty()) return false;
    const std::wstring abs = AbsFor(sp.path);
    bool ok = false, hasWav = false;
    unsigned wch = 0, wrate = 0, wbits = 0;
    std::size_t wsz = 0;
    {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        const audio::Wav* w = GetCached(abs);
        if (w) {
            hasWav = true;
            wch = w->channels; wrate = w->sampleRate; wbits = w->bitsPer;
            wsz = w->data.size();
            const float volF = ((float)gVolumePct / 100.0f) * ((float)sp.vol / 100.0f);
            ok = audio::g_audio.Play(*w, volF, (unsigned)sp.delay, durMsOut);
        }
    }
    if (!hasWav) {
        LogD("MISS: %s (%s)", what.c_str(), strconv::ToUtf8(abs).c_str());
        // 监听位1：连 wem 都没找到也记一笔，方便区分"没匹配条目"与"匹配了但缺文件"
        if (plugin::gWatchMode & 2)
            plugin::Log("[watch/wem] MISS  %s  ->  %s",
                        what.c_str(), strconv::ToUtf8(abs).c_str());
        return false;
    }
    if (ok) {
        LogD("PLAY: %s -> %s | ch=%u rate=%u bits=%u bytes=%zu vol=%d",
             what.c_str(), strconv::ToUtf8(abs).c_str(), wch, wrate, wbits, wsz,
             (int)gVolumePct);
        // 监听位1：WEM 调用流 —— 这条才是"到底响了什么"的确证
        if (plugin::gWatchMode & 2)
            plugin::Log("[watch/wem] PLAY  %s  ->  %s  (ch=%u rate=%u bytes=%zu vol=%d%% delay=%ums)",
                        what.c_str(), strconv::ToUtf8(abs).c_str(),
                        wch, wrate, wsz, (int)gVolumePct, (unsigned)sp.delay);
        return true;
    }
    LogD("waveOut rejected (hr=0x%08X), PlaySoundW fallback: %s",
         (unsigned)audio::g_audio.LastHr(), strconv::ToUtf8(abs).c_str());
    ok = ::PlaySoundW(abs.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT) != FALSE;
    LogD(ok ? "PLAY(fallback): %s" : "PLAYFAIL: %s", strconv::ToUtf8(abs).c_str());
    if (plugin::gWatchMode & 2)
        plugin::Log("[watch/wem] %s  %s  ->  %s",
                    ok ? "PLAY(fb)" : "FAIL", what.c_str(), strconv::ToUtf8(abs).c_str());
    return ok;
}

// Fire one matched entry. Returns true when at least one sound played.
// 播放一个指定的音效池（固定层 + 随机层）。原先这段嵌在 FireEntry 里，
// 拆出来是为了让条件池也能复用同一套播放逻辑。
bool FirePool(const Pool* pool, std::mt19937& rng, std::uint64_t nowMs,
              const std::string& what)
{
    if (!pool || pool->empty()) return false;

    std::vector<int> fixedIdx, randomIdx;
    for (std::size_t i = 0; i < pool->specs.size(); ++i) {
        if (pool->specs[i].fixed) fixedIdx.push_back((int)i);
        else                      randomIdx.push_back((int)i);
    }

    bool any = false;

    // fixed layer: always plays per action, but the same file is still subject
    // to the cooldown /「播放期间不重复」策略，避免重复触发条目把它叠起来。
    for (int i : fixedIdx) {
        const SoundSpec& sp = pool->specs[i];
        const std::string pathKey = strconv::ToUtf8(AbsFor(sp.path));
        if (SpecBlocked(sp, pathKey, nowMs)) {
            LogD("SKIP(fixed): cooldown/playing %s (cd=%d lock=%d)", pathKey.c_str(),
                 sp.cdMs, (int)sp.playLock);
            continue;
        }
        unsigned dur = 0;
        if (PlayOne(sp, what, &dur)) {
            NotePlayed(sp, pathKey, nowMs, dur);
            any = true;
        }
    }

    // random layer: one pick from the non-fixed set; without any fixed sounds
    // this degenerates to the legacy whole-pool random (or first-only) mode.
    const bool hasFixed = !fixedIdx.empty();
    if (hasFixed && randomIdx.empty()) return any;   // only fixed sounds exist

    if (hasFixed) {
        const int n = (int)randomIdx.size();
        const int start = n > 1 ? std::uniform_int_distribution<int>(0, n - 1)(rng) : 0;
        for (int t = 0; t < n; ++t) {
            const SoundSpec& sp = pool->specs[randomIdx[(start + t) % n]];
            const std::string pathKey = strconv::ToUtf8(AbsFor(sp.path));
            if (SpecBlocked(sp, pathKey, nowMs)) continue;
            unsigned dur = 0;
            if (PlayOne(sp, what, &dur)) { NotePlayed(sp, pathKey, nowMs, dur); any = true; break; }
        }
        return any;
    }

    // legacy mode: whole pool is the random set
    const int n = (int)pool->specs.size();
    int start = 0;
    int count = n;
    if (gMoreSounds) {
        if (n > 1) start = std::uniform_int_distribution<int>(0, n - 1)(rng);
    } else {
        count = 1;   // legacy: only the first sound
    }
    for (int t = 0; t < count; ++t) {
        const SoundSpec& sp = pool->specs[(start + t) % n];
        const std::string pathKey = strconv::ToUtf8(AbsFor(sp.path));
        if (SpecBlocked(sp, pathKey, nowMs)) continue;
        unsigned dur = 0;
        if (PlayOne(sp, what, &dur)) { NotePlayed(sp, pathKey, nowMs, dur); any = true; break; }
    }
    return any;
}

bool FireEntry(const Attack& e, int gauge, std::mt19937& rng, std::uint64_t nowMs,
               const std::string& tag)
{
    // choose pool: gauge level pool when present, otherwise default pool
    const Pool* pool = &e.defPool;
    bool usedGauge = false;
    if (gauge >= 0 && gauge <= 3 && !e.gaugePool[gauge].empty()) {
        pool = &e.gaugePool[gauge];
        usedGauge = true;
    }
    const std::string what = tag + (usedGauge ? std::string(" gauge=") + std::to_string(gauge)
                                              : std::string(" gauge=default"));
    return FirePool(pool, rng, nowMs, what);
}

// ★ 事件出口实现：SonarAudio 的日志线程每处理一条播放事件就调这里。
//   命中"按 media id 触发"的条目 → 播放我们的音效（与 fsm 判定并行）。
//
//   与 fsm 判定对齐的两条规矩（wem 是全局音频事件，不做限制会在标题界面就响）：
//     1) **必须在有玩家的场景里**（标题/菜单/加载界面读不到玩家实体 → 不触发）
//     2) **边沿触发**：同一条 wem 连续重复只在"开始"响一次，
//        静默超过 kWemQuietMs 才认为下一轮（不是每次都响）
void WemEventSink(uint32_t media_id, uint32_t event_id, uint32_t playing_id,
                  const char* bank, const char* name) {
    (void)event_id; (void)playing_id; (void)name;
    if (media_id == 0) return;

    // 武器 bank 的 wem 事件 → 武装 layer 探针（下一帧 dump 实体差异）
    if (bank && (bank[0] == 'w' || bank[0] == 'W') && (bank[1] == 'p' || bank[1] == 'P'))
        ArmLayerProbe((int)media_id);

    // 1) 只在真正带玩家的场景里触发（标题界面 / 菜单：gWeapon 读不到 → 直接忽略）
    if (player::gWeapon < 0 || player::gWeapon > 13) return;

    // 1.2) 点名忽略名单（ini WemIgnoreMedia=...）：持续音/环境音直接跳过
    if (!gWemIgnoreMedia.empty() && WemIsIgnored((long long)media_id)) return;

    // 1.3) 场景过滤（仅任务中 / 地图白名单 / 地图黑名单）—— 压掉集会、换区、菜单的误触发
    if (!WemScenePermitted()) return;

    const std::uint64_t now = ::GetTickCount64();

    // 1.5) 场景/装备稳定窗口：进图/换区/切装完成后的一小段时间内，游戏会
    //      预先 PostEvent 一批音效（并非真的在播）。这段窗口内的 wem 一律忽略。
    if (gKillSettleMs > 0 && player::gStableSinceMs != 0 &&
        now - player::gStableSinceMs < (std::uint64_t)gKillSettleMs) {
        return;
    }

    // 1.6) 爆发批次抑制：真正的动作一次通常只发 1~3 条 wem；而预加载会在一瞬间
    //      连发十几条。统计同一 60ms 窗口内进来的事件数，超阈值则认为整批是
    //      预加载，直接丢弃（含当前这条）。
    {
        static std::uint64_t s_burstStart = 0;
        static int           s_burstCount = 0;
        if (now - s_burstStart > 60) { s_burstStart = now; s_burstCount = 0; }
        ++s_burstCount;
        if (gWemBurstCap > 0 && s_burstCount > gWemBurstCap) {
            LogD("[wem] 批次抑制(第%d条/60ms) media=%u", s_burstCount, media_id);
            return;
        }
    }

    // 1.7) 循环音过滤：有些 wem 是"持久循环音"（例：wp_bow_cmn/10 = 182089195，
    //      约每秒一次、持续整场），它们不是动作事件，必须挡住 —— 否则会误触发
    //      条目、也把捕获面板刷屏（用户反馈的"解瓶也捕获到 10""两次 10"就是它）。
    //      判据用**行为**（wem 数据里没有 loop 标志，HIRC 里才有）：
    //        同一 media 在 4 秒内出现 >= gWemLoopRate 次 → 判为循环音，
    //        随后 gWemLoopMuteMs 内直接忽略（真动作不可能 4 秒内重复 4 次同一 wem）。
    if (gWemLoopFilter) {
        struct LoopStat { std::uint64_t winStart = 0; int count = 0; std::uint64_t muteUntil = 0; };
        static std::unordered_map<uint32_t, LoopStat> s_loop;   // 仅日志线程
        LoopStat& ls = s_loop[media_id];
        if (now < ls.muteUntil) return;                          // 已判为循环音，静默期内忽略
        if (now - ls.winStart > 3000) { ls.winStart = now; ls.count = 0; }
        if (++ls.count >= (int)gWemLoopRate) {
            ls.muteUntil = now + (std::uint64_t)gWemLoopMuteMs;
            LogD("[wem] 判定循环音并忽略 media=%u（4s 内第 %d 次）", media_id, ls.count);
            return;
        }
        if (s_loop.size() > 4096) s_loop.clear();
    }

    // 2) 边沿锁（按媒体 id）：连续重复事件只在第一下触发，静默后才重置
    struct WemEdge { std::uint64_t lastMs = 0; bool fired = false; };
    static std::unordered_map<uint32_t, WemEdge> s_edges;   // 仅在日志线程访问
    const std::uint64_t quiet = 1200;   // 同一条 wem 静默 1.2s 才算"下一轮"（游戏会重复 PostEvent）
    WemEdge& ed = s_edges[media_id];
    if (now - ed.lastMs > quiet) ed.fired = false;          // 静默足够久 → 新一轮
    ed.lastMs = now;
    if (ed.fired) return;                                    // 本轮已响过
    if (s_edges.size() > 4096) s_edges.clear();              // 保险：别无限增长

    if ((long long)media_id == gWemLastId && now - gWemLastPlayMs < 300) return;  // 双保险
    static std::mt19937 s_rng((unsigned)(::GetTickCount64() ^ 0x9E3779B9u));
    std::lock_guard<std::mutex> lk(gCfgMutex);
    for (auto& e : gAttacks) {
        if (e.media != (long long)media_id) continue;
        const int w = WemWeaponFromBank(bank ? bank : "");
        if (!WeaponPermitted(e.weaponType, w)) continue;       // 武器触发开关（默认关）
        if (e.weaponType >= 0 && w >= 0 && e.weaponType != w) continue;
        // ★ 可选附加条件：条目填了 LMT / FSMId / FSMTarget 时，要求当前玩家状态也匹配。
        //   同一个 event 在非目标场景也会被请求（进集会/切装备），靠这些条件精准限定；
        //   留空（-1 / 不限）则只看 media。
        if (!e.lmt.empty()) {
            bool ok = false;
            for (int x : e.lmt) if (x == player::gLmt) { ok = true; break; }
            if (!ok) continue;
        }
        if (e.fsmId >= 0 && e.fsmId != player::gFsm) continue;
        if (e.fsmTarget >= 0 && e.fsmTarget != player::gFsmTarget) continue;
        if (e.stop) {
            audio::StopAll();
            gWemLastId = (long long)media_id;
            gWemLastPlayMs = now;
            ed.fired = true;
            LogD("[wem] stop %s (media=%u)", e.name.c_str(), media_id);
            break;
        }
        if (now - e.lastTriggerMs < (std::uint64_t)gDebounceMs) break;   // 每条目独立防抖
        const std::string tag = "a[" + e.name + "] media=" + std::to_string(media_id);
        if (FireEntry(e, player::gGauge, s_rng, now, tag)) {
            e.lastTriggerMs = now;
            gWemLastId = (long long)media_id;
            gWemLastPlayMs = now;
            ed.fired = true;
            LogD("[wem] fired %s (media=%u bank=%s)", e.name.c_str(), media_id,
                 bank ? bank : "?");
        }
        break;
    }
}

// ---------------------------------------------------------------------------
//  延迟判定条目：动作匹配上只是"开窗"，然后盯一段时间再决定播哪个池。
//
//  ★ 窗口**按时间**走完，不因为动作 ID 变了就提前关。
//    游戏里同一招的后续伤害经常落在另一个动作 ID 上（大剑真蓄第一段 1.1 秒就结束，
//    大伤害 1.7~2.1 秒才到账），按 ID 关窗会漏判；按时间兜底则即使遇到没登记过的
//    后续动作 ID 也判得对。
// ---------------------------------------------------------------------------
void TickJudgeEntry(Attack& e, bool match, std::uint64_t nowMs,
                    std::mt19937& rng, const std::string& tag)
{
    if (!e.winOpen) {
        // 只在"不匹配 -> 匹配"的上升沿开窗，否则动作还没放完就会反复开
        if (match && !e.inMatch) {
            e.winOpen    = true;
            e.winStart   = nowMs;
            e.actEnded   = false;
            e.actEndAt   = 0;
            e.dmgPrev    = 0;
            e.dmgHits    = 0;
            e.baseTaken  = (e.checkDelayMs <= 0);
            e.winDmg0    = player::gQuestDmg;
            e.winHp0     = player::gMonHp;
            e.hpDmg      = 0;
            e.baseAura   = player::gGauge;
            e.baseCharge = player::gCharge;
            LogD("%s judge: window open (dmg0=%d hp0=%.1f aura=%d charge=%d)",
                 tag.c_str(), e.winDmg0, e.winHp0, e.baseAura, e.baseCharge);
        }
        e.inMatch = match;
        return;
    }
    e.inMatch = match;

    const std::uint64_t el = nowMs - e.winStart;

    // 到 CheckDelayMs 重取一次伤害基准，把招式前段的伤害排除在外
    if (!e.baseTaken && (int)el >= e.checkDelayMs) {
        e.baseTaken = true;
        e.winDmg0   = player::gQuestDmg;
        e.winHp0    = player::gMonHp;
        e.hpDmg     = 0;
    }

    // ---- 窗口内掉血量（血量信号）----
    // 只在开窗/重取基准之后累计；回血（换区回满、队友奶）只抬高基准，不计负数。
    if (e.baseTaken && e.winHp0 >= 0.0f && player::gMonHp >= 0.0f) {
        if (player::gMonHp < e.winHp0) {
            // 血量比基准低 = 掉了这么多（同一只怪；换怪时 plugin 侧已把 gMonHp 重置为 -1）
            const float drop = e.winHp0 - player::gMonHp;
            const int   d    = (int)(drop + 0.5f);
            if (d > e.hpDmg) e.hpDmg = d;
        } else if (player::gMonHp > e.winHp0 + 0.5f) {
            e.winHp0 = player::gMonHp;   // 回血：抬高基准，别把之后的正常掉血算漏
            e.hpDmg  = 0;
        }
    }

    // ---- dmg 变量：两个信号取"较大者" ----
    //   ① QuestDmg（只记你自己的伤害，联机最干净）—— 能读出来时优先。
    //   ② 怪物血量掉幅 —— QuestDmg 读不到（对象没建/换区/练习场）**或读到但一直
    //      不动**（个别版本/场景里它不涨）时兜底。
    //   这正是"自定义判定没生效"的根因：原先只认 QuestDmg，它一旦无效，
    //   dmg 恒为 0，dmg>0 就永远不成立，判定模式的音效自然一声不响。
    //   取 max 而不是"二选一"：有效的那路给出了伤害就用它，另一路是 0 也无妨。
    const bool questOk = (player::gQuestDmg >= 0 && e.winDmg0 >= 0);
    const bool hpOk    = (player::gMonHp >= 0.0f && e.winHp0 >= 0.0f &&
                          player::gMonHpMax >= 10.0f);   // 木桩(1/1)不算真怪
    cond::Vars v{};
    if (e.baseTaken) {
        const int qd = questOk ? (player::gQuestDmg - e.winDmg0) : 0;
        const int hd = hpOk ? e.hpDmg : 0;
        v.v[cond::V_DMG] = (qd > hd) ? qd : hd;
    } else {
        v.v[cond::V_DMG] = 0;
    }

    v.v[cond::V_AURA]    = player::gGauge;
    v.v[cond::V_DAURA]   = (player::gGauge >= 0 && e.baseAura >= 0)
                             ? (player::gGauge - e.baseAura) : 0;
    v.v[cond::V_CHARGE]  = player::gCharge;
    v.v[cond::V_DCHARGE] = (player::gCharge >= 0 && e.baseCharge >= 0)
                             ? (player::gCharge - e.baseCharge) : 0;
    v.v[cond::V_LMT]     = player::gLmt;
    v.v[cond::V_FSM]     = player::gFsm;
    v.v[cond::V_FSMTGT]  = player::gFsmTarget;
    v.v[cond::V_MS]      = (int)el;
#ifdef WSE_PROBE_NO_MAIN
    e.lastDmg = v.v[cond::V_DMG];
#endif

    // 窗口结算时机
    //   EndOn=time  (默认) : 只看 CheckTimeoutMs
    //   EndOn=action       : 动作一离开本条目就算结束，再等 CheckGraceMs 结算；
    //                        CheckTimeoutMs 仍作为上限兜底，防止动作一直不变
    // 调试：窗口里每来一笔伤害就记一行（含时刻和当时的练气变化）。
    // 多段攻击靠这个才能看出"第一段小伤害在第几毫秒、第二段大伤害在第几毫秒"，
    // CheckDelayMs 就卡在两段中间的空档里。最多记 8 笔，防刷屏。
    {
        const int cur = v.v[cond::V_DMG];
        if (e.dmgPrev >= 0 && cur > e.dmgPrev) {
            if ((int)el > e.maxDmgMs) e.maxDmgMs = (int)el;   // 跨窗口累积，只增不减
            if (e.dmgHits < 8) {
                ++e.dmgHits;
                LogD("%s judge: dmg +%d (total %d) at ms=%d  dAura=%d  (最晚出伤 %dms)",
                     tag.c_str(), cur - e.dmgPrev, cur, (int)el,
                     v.v[cond::V_DAURA], e.maxDmgMs);
            }
        }
        e.dmgPrev = cur;
    }

    if (e.checkEndOn == 1 && !match && !e.actEnded) {
        e.actEnded = true;
        e.actEndAt = nowMs;
        LogD("%s judge: action ended at ms=%d (offset %+dms, 最晚出伤 %dms)",
             tag.c_str(), (int)el, e.checkGraceMs, e.maxDmgMs);
    }

    // 判定时刻：两条路径谁先到算谁（offset 保证 >= 0，不会算出负数）
    bool endReached = false;
    if (e.checkEndOn == 1) {
        // ① 动作结束（含被打断）+ offset
        if (e.actEnded && (nowMs - e.actEndAt) >= (std::uint64_t)e.checkGraceMs)
            endReached = true;
        // ② 历史最晚出伤时刻 + offset —— 过了它就不可能再有伤害，不必等后摇
        if (!endReached && e.maxDmgMs >= 0 &&
            (int)el >= e.maxDmgMs + e.checkGraceMs)
            endReached = true;
    }
    const bool timeUp = ((int)el >= e.checkTimeoutMs) || endReached;

    // Sound:    条件一成立就播（定局型，如"掉刃"）
    // SoundEnd: 只在窗口结束时评（可能被推翻的，如"没掉刃且有伤害"）
    // CheckMode=final 等价于把所有条件都标成 SoundEnd。
    for (std::size_t i = 0; i < e.conds.size(); ++i) {
        const CondPool& c = e.conds[i];
        if (c.pool.empty()) continue;
        const bool waitEnd = c.atEnd || e.checkMode == 1;
        if (waitEnd && !timeUp) continue;          // 还没到点，这条先不评
        if (!cond::Eval(c.expr, v)) continue;
        LogD("%s judge: MATCH [%s]%s dmg=%d dAura=%d ms=%d",
             tag.c_str(), c.text.c_str(), waitEnd ? " (end)" : "",
             v.v[cond::V_DMG], v.v[cond::V_DAURA], v.v[cond::V_MS]);
        FirePool(&c.pool, rng, nowMs, tag + " [" + c.text + "]");
        e.winOpen = false;
        return;
    }

    if (timeUp) {
        // 都不成立 -> 兜底池（就是这条目的 Sound= ）
        LogD("%s judge: timeout, fallback (dmg=%d dAura=%d)",
             tag.c_str(), v.v[cond::V_DMG], v.v[cond::V_DAURA]);
        FirePool(&e.defPool, rng, nowMs, tag + " [timeout]");
        e.winOpen = false;
    }
}

// ===========================================================================
//  Worker threads
// ===========================================================================

// GUI 按钮请求：数据目录里出现标记文件 → 立即重载配置 / 停止全部音效（处理完删掉标记）
void HandleGuiRequests()
{
    const std::wstring reloadFlag = gDataDir + L"_wse_reload.flag";
    const std::wstring stopFlag   = gDataDir + L"_wse_stop.flag";
    if (::GetFileAttributesW(reloadFlag.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ::DeleteFileW(reloadFlag.c_str());
        audio::StopAll();
        ReloadConfig();
        Log("GUI request: reload done");
        ShowMessage("wse reloaded (from GUI)", true);
    }
    if (::GetFileAttributesW(stopFlag.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ::DeleteFileW(stopFlag.c_str());
        audio::StopAll();
        Log("GUI request: stop all sounds");
    }
}

DWORD WINAPI WorkerProc(LPVOID)
{
    while (::GetModuleHandleW(L"MonsterHunterWorld.exe") == nullptr &&
           ::InterlockedCompareExchange(&gStop, 0, 0) == 0)
        ::Sleep(500);
    if (gStop) return 0;

    Log("game module found, init audio");
    ResolveGameBase();
    audio::g_audio.Init();
    PreloadSounds();

    std::mt19937 rng(static_cast<unsigned>(::GetTickCount64() ^ 0x9E3779B9u));
    std::uint64_t lastHeartbeat = 0;
    bool firstState = true;

    while (::InterlockedCompareExchange(&gStop, 0, 0) == 0) {
        ::Sleep(static_cast<DWORD>(gPollMs));
        if (::InterlockedCompareExchange(&gStop, 0, 0) != 0) break;

        player::Refresh();
        const int lmt = player::gLmt;
        const int fsm = player::gFsm;
        const int weapon = player::gWeapon;
        const int gauge = player::gGauge;

        // -------------------------------------------------------------------
        //  监听位 0：派生 ID 流
        //  只在 FSM / FSM层 / LMT / 武器 与上一帧不同时打一行，避免刷屏。
        //  同时尽量反查出"这个派生叫什么名字"——遍历配置里的条目，找一条
        //  武器/FSM/FSM层/LMT 都能对上的，取它的 name 作为参考动作名。
        //  （纯显示用途；即使没配条目也会照打，方便勘察未收录的派生。）
        // -------------------------------------------------------------------
        if (gWatchMode & 1) {
            const int fsmTgt = player::gFsmTarget;
            if (fsm != gWatchLastFsm || fsmTgt != gWatchLastFsmTgt ||
                lmt != gWatchLastLmt || weapon != gWatchLastWeapon) {

                // 反查动作名：取第一个基础匹配的条目名
                std::string nameHint;
                {
                    std::lock_guard<std::mutex> lk(gCfgMutex);
                    for (const auto& e : gAttacks) {
                        if (e.weaponType >= 0 && e.weaponType != weapon) continue;
                        if (e.fsmId >= 0 && e.fsmId != fsm) continue;
                        if (e.fsmTarget >= 0 && e.fsmTarget != fsmTgt) continue;
                        if (!e.lmt.empty()) {
                            bool ok = false;
                            for (int x : e.lmt) if (x == lmt) { ok = true; break; }
                            if (!ok) continue;
                        }
                        nameHint = e.name;
                        break;
                    }
                }

                Log("[watch/derived] weapon=%d fsm=%d fsmtgt=%d lmt=%d gauge=%d  %s",
                    weapon, fsm, fsmTgt, lmt, gauge,
                    nameHint.empty() ? "(无匹配条目)" : nameHint.c_str());

                gWatchLastFsm    = fsm;
                gWatchLastFsmTgt = fsmTgt;
                gWatchLastLmt    = lmt;
                gWatchLastWeapon = weapon;
            }
        }

        PollChatCommand();
        HandleGuiRequests();   // GUI 按钮写的标记文件 → 立即重载 / 停止音效
        ::sonarefx::Flush();   // 特效捕获：把 hook 记下的事件查表 + 写日志（主线程做 I/O）
        DumpLayerProbe();      // layer 探针（ini LayerProbe=1 时）：wem 事件后 dump 实体差异

        // 场景小文件（GUI 捕获面板显示当前场景；顺便给"判定信号可用性"）：
        //   mapId / questState / 任务累计伤害 / 怪物血量 / 血量上限
        //   GUI 靠后三个判断"伤害类判定现在能不能生效"（训练场/非任务读不到伤害）。
        {
            static int s_lastMap = -999, s_lastQuest = -999;
            static std::uint64_t s_lastSceneWrite = 0;
            const std::uint64_t nowScene = ::GetTickCount64();
            if (player::gMapId != s_lastMap || player::gQuestState != s_lastQuest ||
                nowScene - s_lastSceneWrite >= 1000) {
                s_lastMap = player::gMapId;
                s_lastQuest = player::gQuestState;
                s_lastSceneWrite = nowScene;
                char buf[192] = {};
                _snprintf_s(buf, _TRUNCATE, "%d\n%d\n%d\n%.1f\n%.1f",
                            player::gMapId, player::gQuestState,
                            player::gQuestDmg, player::gMonHp, player::gMonHpMax);
                FILE* f = nullptr;
                if (_wfopen_s(&f, (gModuleDir + L"SonarScene.txt").c_str(), L"wb") == 0 && f) {
                    fwrite(buf, 1, strlen(buf), f);
                    fclose(f);
                }
            }
        }

        const std::uint64_t nowMs = ::GetTickCount64();
        if (firstState || (nowMs - lastHeartbeat >= 4000)) {
            LogD("state: weapon=%d fsm=%d lmt=%d gauge=%d vol=%d en=%d more=%d map=%d quest=%d",
                 weapon, fsm, lmt, gauge, gVolumePct, gEnabled, gMoreSounds,
                 player::gMapId, player::gQuestState);
            lastHeartbeat = nowMs;
            firstState = false;
        }

        if (gEnabled == 0) continue;

        // wem 触发：主通道 = 合并进来的 Wwise hook（事件经 WemEventSink 直接触发）；
        // 兜底通道 = 读 SonarAudio.log（默认关，避免同一事件被触发两次）
        if (g_useLogWemFallback.load()) PollWemEvents(rng);

        // Iterate the REAL container under the config lock. Entry latches
        // (inMatch) must persist across polls or the same action would be
        // re-fired on every poll once DebounceMs elapses. Reload swaps the
        // vector under the same lock, so iteration stays safe.
        {
            std::lock_guard<std::mutex> lk(gCfgMutex);

            // Re-arm action groups whose members have been quiet long enough.
            for (auto& g : g_groups) {
                if (g.second.fired && nowMs - g.second.lastSeen >= kGroupResetMs)
                    g.second.fired = false;
            }

            for (auto& e : gAttacks) {
                bool lmtOk = e.lmt.empty();
                if (!lmtOk)
                    for (int x : e.lmt) if (x == lmt) { lmtOk = true; break; }

                // 去掉 HasSounds 的“基础匹配”：Stop 条目没有音效也要能匹配
                // ★ 外加武器触发开关（默认全关）：该武器没勾 → 一律不匹配
                const bool matchBase =
                    WeaponPermitted(e.weaponType, weapon) &&
                    (e.weaponType < 0 || e.weaponType == weapon) &&
                    (e.fsmId < 0 || e.fsmId == fsm) &&
                    (e.fsmTarget < 0 || e.fsmTarget == player::gFsmTarget) &&
                    lmtOk;

                // Stop 条目：命中该派生动作就停止正在播放的音效（边沿触发，不进组/判定）
                if (e.stop) {
                    if (matchBase && !e.inMatch) {
                        e.inMatch = true;
                        audio::StopAll();
                        LogD("[stop] %s (weapon=%d fsm=%d lmt=%d)",
                             e.name.c_str(), weapon, fsm, lmt);
                    } else if (!matchBase) {
                        e.inMatch = false;
                    }
                    continue;
                }

                const bool match = matchBase && e.HasSounds();
                const std::string tag = "a[" + e.name + "]";

                // 延迟判定条目走独立的状态机；没配 CheckTimeoutMs 的条目
                // 一律走下面的旧逻辑，行为与旧版完全一致。
                if (e.checkTimeoutMs > 0 && !e.conds.empty()) {
                    TickJudgeEntry(e, match, nowMs, rng, tag);
                    continue;
                }

                if (!e.group.empty()) {
                    // Action-group entry: the whole group fires at most once
                    // per action occurrence. Gauge is taken at the first
                    // trigger instant, so a mid-action gauge change cannot
                    // double-fire.
                    GroupRt& st = GroupState(e.group);
                    if (match) st.lastSeen = nowMs;
                    if (!match || st.fired) continue;
                    st.fired = true;
                    if (nowMs - st.lastTriggerMs < (std::uint64_t)gDebounceMs)
                        continue;
                    if (FireEntry(e, gauge, rng, nowMs, tag))
                        st.lastTriggerMs = nowMs;
                    continue;
                }

                // Ungrouped entry: legacy per-entry edge-latch behaviour.
                if (match && !e.inMatch) {
                    e.inMatch = true;
                    if (nowMs - e.lastTriggerMs < (std::uint64_t)gDebounceMs)
                        continue;
                    if (FireEntry(e, gauge, rng, nowMs, tag))
                        e.lastTriggerMs = nowMs;
                } else if (!match) {
                    e.inMatch = false;
                }
            }
        }
    }
    return 0;
}

DWORD WINAPI HotkeyProc(LPVOID)
{
    bool reloadWas=false, upWas=false, downWas=false,
         setWas=false, togWas=false, moreWas=false, comboWas=false;
    while (::InterlockedCompareExchange(&gStop, 0, 0) == 0) {
        ::Sleep(30);
        if (g_hotkeysEnabled == 0) {
            reloadWas = upWas = downWas = setWas = togWas = moreWas = comboWas = false;
            continue;
        }
        const bool mod = (gModifierKey == 0) ||
                         ((::GetAsyncKeyState(gModifierKey) & 0x8000) != 0);
        if (!mod) {
            reloadWas = upWas = downWas = setWas = togWas = moreWas = comboWas = false;
            continue;
        }
        const bool reloadDown = (::GetAsyncKeyState(gReloadKey) & 0x8000) != 0;
        const bool upDown     = (::GetAsyncKeyState(gVolUpKey)  & 0x8000) != 0;
        const bool downDown   = (::GetAsyncKeyState(gVolDownKey)& 0x8000) != 0;
        const bool setDown    = (::GetAsyncKeyState(gSetVolKey) & 0x8000) != 0;
        const bool togDown    = (::GetAsyncKeyState(gToggleKey) & 0x8000) != 0;
        const bool moreDown   = (::GetAsyncKeyState(gMoreKey)   & 0x8000) != 0;
        const bool comboDown  = (::GetAsyncKeyState(gComboKey)  & 0x8000) != 0;

        if (reloadDown && !reloadWas) {
            ReloadConfig();
            char m[0x180] = {};
            _snprintf_s(m, _TRUNCATE, "wse reloaded (vol=%d)", (int)gVolumePct);
            ShowMessage(m, true);
            LogD("[hotkey] config reloaded (volume=%d enabled=%d more=%d)",
                 gVolumePct, gEnabled, gMoreSounds);
        }
        reloadWas = reloadDown;

        if (upDown && !upWas) {
            gVolumePct += 5; if (gVolumePct > 100) gVolumePct = 100;
            char m[0x180] = {}; _snprintf_s(m, _TRUNCATE, "wse volume=%d", (int)gVolumePct);
            ShowMessage(m, true);
            LogD("[hotkey] volume up -> %d", gVolumePct);
        }
        upWas = upDown;

        if (downDown && !downWas) {
            gVolumePct -= 5; if (gVolumePct < 0) gVolumePct = 0;
            char m[0x180] = {}; _snprintf_s(m, _TRUNCATE, "wse volume=%d", (int)gVolumePct);
            ShowMessage(m, true);
            LogD("[hotkey] volume down -> %d", gVolumePct);
        }
        downWas = downDown;

        if (setDown && !setWas) {
            gVolumePct = gSetVolValue;
            char m[0x180] = {}; _snprintf_s(m, _TRUNCATE, "wse volume=%d", (int)gVolumePct);
            ShowMessage(m, true);
            LogD("[hotkey] volume set -> %d", gVolumePct);
        }
        setWas = setDown;

        if (togDown && !togWas) {
            gEnabled = gEnabled ? 0 : 1;
            ShowMessage(gEnabled ? "wse enabled" : "wse disabled", true);
            LogD("[hotkey] enabled toggled -> %d", gEnabled);
        }
        togWas = togDown;

        if (moreDown && !moreWas) {
            gMoreSounds = gMoreSounds ? 0 : 1;
            ShowMessage(gMoreSounds ? "wse extra sounds ON" : "wse extra sounds OFF", true);
            LogD("[hotkey] more-sounds toggled -> %d", gMoreSounds);
        }
        moreWas = moreDown;

        // 切换当前武器的配置组合（仅影响该武器；其它武器配置不变）
        if (comboDown && !comboWas) {
            const int w = player::gWeapon;
            std::string nxt, err;
            if (w < 0) {
                err = "未进入场景，无法切换组合";
            } else if (ComboNext(w, nxt, err)) {
                PersistActiveCombo(w, nxt);
                const std::vector<std::string> order = ComboList(w);
                std::string m = "wse combo -> " + ComboDisplayName(nxt);
                if (order.size() <= 1)
                    m += "（该武器只有 1 个组合，GUI 里可「新增组合」）";
                else
                    m += "（" + std::to_string(order.size()) + " 个可用）";
                ShowMessage(m.c_str(), true);
                LogD("[hotkey] weapon %d combo -> [%s] (%zu combos)", w, nxt.c_str(), order.size());
            }
            if (!err.empty()) {
                std::string m = "wse combo: " + err;
                ShowMessage(m.c_str(), true);
                Log("[hotkey] combo switch failed: %s", err.c_str());
            }
        }
        comboWas = comboDown;
    }
    return 0;
}

} // namespace plugin

// ===========================================================================
//  DLL entry point
// ===========================================================================
extern "C" __declspec(dllexport) BOOL WeaponSoundEnhance_IsInstalled() { return TRUE; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    using namespace plugin;
    if (reason == DLL_PROCESS_ATTACH) {
        gModule = module;
        ::DisableThreadLibraryCalls(module);
        wchar_t self[MAX_PATH] = {};
        ::GetModuleFileNameW(module, self, MAX_PATH);
        const std::wstring selfw(self);
        std::size_t sl = selfw.find_last_of(L"\\/");
        gModuleDir = (sl == std::wstring::npos) ? std::wstring() : selfw.substr(0, sl + 1);
        gDataDir  = ResolveDataDir();
        gIniPath  = gDataDir + L"WeaponSoundEnhance.ini";
        gWemLogPath = gModuleDir + L"SonarAudio.log";   // 与 DLL 同在 plugins\（SonarAudio hook 输出）
        LogInit();
        SeedIniFromTemplate();
        LoadConfig();
        // ★ 合并进来的 Wwise 捕获模块：解析导出表 + MinHook 三 hook + 扫 nbnk 反查表，
        //   事件经 WemEventSink 直接触发 wem 条目（不再依赖单独的 SonarAudio.dll）。
        //   注意：hook 若失败会自动降级（日志里会写），触发退回读磁盘日志通道。
        ::sonaraudio::SetEventSink(&WemEventSink);
        ::sonaraudio::SonarAudioStart(module);
        // ★ 特效捕获（实验性）：运行时扫内存定位 Effect:Ctor → hook → 记录 (表,组,记录)。
        //   磁盘上的 exe 被 DRM 包裹（.text 全加密），所以只能扫内存；扫不到会在日志里写明原因。
        {
            std::string efxLog;
            // ★ 默认关闭：只有 ini 里 EfxCapture=1 才初始化（否则不扫内存、不挂钩，零开销）
            if (gEfxCapture) {
                ::sonarefx::SetWeaponProvider(&EfxWeaponProvider);
                ::sonarefx::SetEnabled(true);
                ::sonarefx::SetLogAll(gEfxLogAll != 0);
                ::sonarefx::SetDiscover(gEfxDiscover != 0);
                ::sonarefx::SetTablePrefs(gEfxTablePref);
                ::sonarefx::Init(gModuleDir, efxLog);
                if (!efxLog.empty()) LogD("%s", efxLog.c_str());
            } else {
                efxLog = "[efx] 已关闭（EfxCapture=0）：不扫描内存、不挂钩";
                LogD("%s", efxLog.c_str());
            }
        }
        HANDLE t = ::CreateThread(nullptr, 0, &WorkerProc, nullptr, 0, nullptr);
        if (t) ::CloseHandle(t);
        HANDLE hk = ::CreateThread(nullptr, 0, &HotkeyProc, nullptr, 0, nullptr);
        if (hk) ::CloseHandle(hk);
    } else if (reason == DLL_PROCESS_DETACH) {
        ::sonaraudio::SonarAudioStop();
        ::InterlockedExchange(&gStop, 1);
        ::Sleep(1000);
    }
    return TRUE;
}
