#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 一条音效（音效组内的一项）
struct SoundSpec {
    std::string path;     // 相对路径，如 sounds/xxx.wav
    int delay = 0;        // 播放延时(ms)
    int vol = 100;        // 0..100；100 = 相对主音量无额外调整
    bool fixed = false;   // |F 固定：命中该池时恒播；同池未固定者仍随机抽一条
    int  cdMs = 0;        // |C<ms> 该音效自身的再触发冷却（0 = 用默认 400ms）
    bool playLock = false;// |P 播放期间不再被重复触发（播放中/延时中都算）
};

// 一个音效组（默认音效或某刃时音效）
struct PoolSpec {
    std::vector<SoundSpec> specs;
    bool empty() const { return specs.empty(); }
};

// 一条「条件 -> 音效池」。判定窗口内按书写顺序求值，第一个成立的播。
struct CondSpec {
    std::string expr;       // 条件表达式，如 "dmg>0 & dAura>=0"
    bool atEnd = false;     // true 写成 SoundEnd:（只在窗口结束时评），false 写成 Sound:
    PoolSpec pool;
};

// 一条派生攻击音效条目
struct SoundEntry {
    int weaponType = -1;        // 0..13，-1 = 任意武器
    std::vector<int> lmt;       // 触发的 LMT 列表；空 = 不限
    int fsmId = -1;             // 动作状态机 ID，-1 = 不限
    int fsmTarget = -1;         // FSMTarget= 目标层；-1 = 不限定
    long long media = -1;       // WEM/media id（WWise）：>0 时用"游戏播放该 wem"触发，
                                // 与 fsm/lmt 判定二选一。-1 = 不按 media 判定
                                // （从 wem 捕获历史添加的条目会带这个值）
    std::string bank;           // 归属的 nbnk bank 名（wp_bow_cmn），wem 条目显示用
    std::string combo;          // 所属配置组合名（"" = 默认组合）
    std::string name;           // 显示名（Name=，插件匹配时忽略）
    std::string group;          // 动作组（Group=）：同一招的多个触发条目填相同组名，
                                // 整招只响一次且刃色在首个触发瞬间定格
    bool stop = false;          // Stop=1：命中这条时停止正在播放的音效（不需要音效）
    // ★ 蓄力等级条目（ChargeLevel=1/2/3）：这条专门负责"蓄到第 N 段"的音效。
    //   触发 = 蓄力升到该段的瞬间（边沿），仍受 Weapon/FSM/LMT 过滤 →
    //   同一段可以在不同招式/过渡里配不同音效（派生进度细分）。0 = 普通条目。
    int chargeLevel = 0;
    bool chargeCut = false;     // ChargeCut=1：本段响时先停掉上一段蓄力音效
    PoolSpec def;               // 默认音效（Sound=）；启用判定时它是"都不成立"的兜底池
    PoolSpec gauge[4];          // 刃时音效 0..3（无/白/黄/红；Sound:none|white|yellow|red）
    PoolSpec charge[4];         // 蓄力等级音效 0..3（大剑/弓/锤；Sound:charge0..3，1=蓄力1段…3=蓄满）

    // ---- 延迟判定（checkTimeoutMs > 0 且 conds 非空时启用）----
    // 动作匹配上只是开窗，接着盯一段时间，按条件挑音效池。
    // 用来做"打中/落空""掉刃/升刃"这类必须观察一段时间才知道结果的触发。
    int checkDelayMs = 0;         // 从第几毫秒开始计伤害（排除招式前段的伤害）
    int checkTimeoutMs = 0;       // 窗口上限；0 = 不启用判定，行为与旧版一致
    int checkOffsetMs = 150;      // 判定点在"实测最晚出伤时刻"之上留的余量
    bool endOnAction = false;   // 默认不勾（用户实测"莫名其妙会勾上"）；旧配置 ini 里有 CheckEndOn=action 的仍为 true      // 动作结束(含被打断)也作为判定时机
    int checkMode = 0;            // CheckMode=final(1)：把所有条件都改成窗口结束时评
    int judgePreset = 0;          // 判定模式下拉的选择（0=不判定 1=造成伤害 2=登龙 3=大居 4=真蓄 5=自定义）
    std::vector<CondSpec> conds;

    int LmtAny() const { return lmt.empty() ? -1 : lmt.front(); }
    bool MatchesLmt(int v) const {
        if (v < 0 || lmt.empty()) return true;
        for (int x : lmt) if (x == v) return true;
        return false;
    }
};

struct GlobalSettings {
    std::string playerRoot = "0x1450139A0";
    int pollMs = 60;
    int debounceMs = 120;
    int volume = 50;
    int enabled = 1;
    int moreSounds = 1;         // 旧全局开关：仅对无固定音效的纯旧条目生效
    // 特效捕获（实验性）：与 DLL 的 sonarefx 模块对应
    int efxCapture = 1;          // 0 = 关闭特效捕获
    int efxLogAll = 1;           // 1 = 把不在索引里的原始调用也写进日志（诊断）
    int efxDiscover = 1;         // 1 = 挂钩 provider 虚表槽，找真正的特效入口（诊断）
    std::string gaugePtrOff = "0x76B0";   // 太刀气刃对象偏移（十六进制）
    std::string gaugeValOff = "0x2370";   // 气刃等级偏移（十六进制）
    std::string chargeValOff = "0x2358";  // 大剑蓄力等级偏移（十六进制）
    std::string fsmTargetOff = "0x6274";  // FSM target 偏移（十六进制）
    std::string questRoot = "0x14500ED30";// 任务结构入口（十六进制）
    std::string questDmgOff = "0x17088";  // 任务累计伤害偏移（十六进制）
    int debug = 0;              // 调试日志（插件侧 Debug=1）
    int chatEcho = 1;
    int updateCheck = 1;        // GUI 启动时自动查一次新版（UpdateCheck=0 关闭）
    std::string updateProxy;    // 走代理查更新时填，如 http://127.0.0.1:7897
    int chatCommands = 1;
    int hotkeysEnabled = 1;     // 启用热键（插件侧 Hotkeys=1）
    // ---- 武器触发开关 / wem 场景过滤（★ 放这里才会被 SaveConfig 保留）----
    std::string weaponEnabled;  // "0,1,0,..." 14 位（武器 0..13）
    int     wemOnlyInQuest = 0; // 仅任务中触发 wem
    std::string wemMapWhite;    // 地图 id 白名单（逗号分隔；空 = 不限）
    std::string wemMapBlack;    // 地图 id 黑名单
    std::string bankOutDir;     // nbnk 导出目录（空 = plugins\WeaponSoundEnhance\wemmod）
};

struct Hotkeys {
    int modifierKey = 17;   // Ctrl
    int reloadKey = 116;    // F5
    int volUpKey = 38;      // Up
    int volDownKey = 40;    // Down
    int setVolKey = 119;    // F8
    int setVolValue = 50;
    int toggleKey = 120;    // F9
    int moreKey = 121;      // F10
    int comboKey = 122;     // F11 切换当前武器配置组合
};

struct Config {
    GlobalSettings global;
    Hotkeys hotkeys;
    std::map<int, std::string> active;   // 每武器当前组合名（""=默认）
    // 每武器的组合名列表（不含 ""，顺序 = 显示顺序）。
    // ★ 空组合也要在这里注册，否则新组合（还没条目）会被"按条目推导组合"
    //   的函数丢掉，表现为"新增组合没反应"。持久化在 ini 的 [Combos] 段。
    std::map<int, std::vector<std::string>> comboList;
    std::vector<SoundEntry> entries;     // 所有组合的条目（每条带 combo 标记）
    std::string path;
    bool loaded = false;
    // 读盘时修正的"组合归属错位"条目数（旧版本保存会把默认组合的条目写进
    // 上一个命名组合的段里，这里读到就自动改回默认组合，见 SaveConfig 的修复）
    int fixedCombos = 0;
};

bool LoadConfig(const std::string& path, Config& cfg);
bool SaveConfig(const std::string& path, const Config& cfg);
// 导出单个组合为可分享的 ini 片段（只含该组合的条目）。err 带失败原因。
bool ExportComboFile(const std::string& path, int weapon, const std::string& combo,
                     const std::vector<SoundEntry>& all, std::string& err);
// 生成组合导出的文本内容（带 UTF-8 BOM，供写入文件/打进 zip 用）。空组合返回 false。
bool BuildComboExportText(int weapon, const std::string& combo,
                          const std::vector<SoundEntry>& all,
                          std::string& outText, std::string& err);

const char* WeaponName(int t);
// 刃时标签：tag ∈ none|white|yellow|red 或 0..3 → 0..3，否则 -1
int GaugeTagIndex(const std::string& tag);
const char* GaugeTagName(int level);   // level 0..3 -> "none".."red"
const char* GaugeUiName(int level);    // level 0..3 -> "无刃".."红刃"
std::uint64_t ParsePlayerRoot(const std::string& s, std::uint64_t defval);
