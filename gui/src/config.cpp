#include "config.h"
#include "fsutil.h"
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string ToLower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

// 以 ; , 分隔
std::vector<std::string> SplitList(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i <= v.size(); ++i) {
        char c = (i < v.size()) ? v[i] : '\0';
        if (c == '\0' || c == ';' || c == ',') {
            std::string t = Trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
            if (c == '\0') break;
        } else {
            cur += c;
        }
    }
    return out;
}

// path [ | delay | vol | flags ]，flags 含 F 表示固定
void ParseSpec(const std::string& token, SoundSpec& sp) {
    sp = SoundSpec{};
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= token.size(); ++i) {
        char c = (i < token.size()) ? token[i] : '\0';
        if (c == '\0' || c == '|') {
            parts.push_back(Trim(cur));
            cur.clear();
            if (c == '\0') break;
        } else {
            cur += c;
        }
    }
    sp.path = parts.empty() ? "" : parts[0];
    if (parts.size() >= 4) {
        sp.delay = parts[1].empty() ? 0 : std::atoi(parts[1].c_str());
        if (sp.delay < 0) sp.delay = 0;
        int v = parts[2].empty() ? 100 : std::atoi(parts[2].c_str());
        sp.vol = v < 0 ? 0 : (v > 200 ? 200 : v);   // 单条音效音量 0..200
        // 第 4 段是标志位：可组合 F(固定) / C<ms>(自身冷却) / P(播放中不重复)
        std::string fl = ToLower(parts[3]);
        sp.fixed = fl.find('f') != std::string::npos;
        sp.playLock = fl.find('p') != std::string::npos;
        {
            const size_t cp = fl.find('c');
            if (cp != std::string::npos) {
                int n = 0; size_t i = cp + 1;
                while (i < fl.size() && fl[i] >= '0' && fl[i] <= '9') { n = n * 10 + (fl[i] - '0'); ++i; }
                if (n < 0) n = 0;
                if (n > 60000) n = 60000;
                sp.cdMs = n;
            }
        }
    } else if (parts.size() == 3) {
        sp.delay = parts[1].empty() ? 0 : std::atoi(parts[1].c_str());
        if (sp.delay < 0) sp.delay = 0;
        int v = parts[2].empty() ? 100 : std::atoi(parts[2].c_str());
        sp.vol = v < 0 ? 0 : (v > 200 ? 200 : v);   // 单条音效音量 0..200
    } else if (parts.size() == 2) {
        sp.delay = parts[1].empty() ? 0 : std::atoi(parts[1].c_str());
        if (sp.delay < 0) sp.delay = 0;
    }
}

// 追加解析一行的音效到池；纯旧式 token 登记进 plainOrder 供 SoundDelay/SoundVol 覆盖
void AppendSpecs(PoolSpec& pool, const std::string& value,
                 std::vector<std::pair<PoolSpec*, int>>& plainOrder) {
    for (const auto& tok : SplitList(value)) {
        SoundSpec sp;
        ParseSpec(tok, sp);
        if (sp.path.empty()) continue;
        bool dup = false;
        for (const auto& ex : pool.specs)
            if (ex.path == sp.path) { dup = true; break; }
        if (dup) continue;
        const bool wasPlain = (tok.find('|') == std::string::npos);
        const int idx = (int)pool.specs.size();
        pool.specs.push_back(sp);
        if (wasPlain) plainOrder.emplace_back(&pool, idx);
    }
}

} // namespace

const char* WeaponName(int t) {
    switch (t) {
        case 0:  return "大剑";
        case 1:  return "片手";
        case 2:  return "双刀";
        case 3:  return "太刀";
        case 4:  return "大锤";
        case 5:  return "笛子";
        case 6:  return "长枪";
        case 7:  return "铳枪";
        case 8:  return "斩斧";
        case 9:  return "盾斧";
        case 10: return "虫棍";
        case 11: return "弓箭";
        case 12: return "重弩";
        case 13: return "轻弩";
        default: return "任意";
    }
}

int GaugeTagIndex(const std::string& tag) {
    if (tag.empty()) return -1;
    if (tag.size() == 1 && tag[0] >= '0' && tag[0] <= '3') return tag[0] - '0';
    const char* names[4] = {"none", "white", "yellow", "red"};
    for (int i = 0; i < 4; ++i)
        if (ToLower(tag) == names[i]) return i;
    return -1;
}

const char* GaugeTagName(int level) {
    static const char* names[4] = {"none", "white", "yellow", "red"};
    return (level >= 0 && level <= 3) ? names[level] : "?";
}

const char* GaugeUiName(int level) {
    static const char* names[4] = {"无刃", "白刃", "黄刃", "红刃"};
    return (level >= 0 && level <= 3) ? names[level] : "?";
}

std::uint64_t ParsePlayerRoot(const std::string& s, std::uint64_t defval) {
    std::string v = Trim(s);
    if (v.empty()) return defval;
    const char* p = v.c_str();
    while (*p == ' ' || *p == '\t') ++p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    if (!*p) return defval;
    char* end = nullptr;
    unsigned long long hv = std::strtoull(p, &end, 16);
    if (end == p) return defval;
    return static_cast<std::uint64_t>(hv);
}

static int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool LoadConfig(const std::string& path, Config& cfg) {
    std::string txt;
    // 宽路径读取：路径含中文时 std::ifstream 会把 UTF-8 当 ANSI，直接变成“文件不存在”
    if (!FsRead(path, txt)) return false;

    if (txt.size() >= 3 && (unsigned char)txt[0] == 0xEF &&
        (unsigned char)txt[1] == 0xBB && (unsigned char)txt[2] == 0xBF)
        txt = txt.substr(3);

    cfg.entries.clear();
    cfg.active.clear();
    cfg.fixedCombos = 0;
    std::map<int, std::string> activeMap;
    std::string section;
    bool inAttack = false, inCombo = false;
    int curComboW = -1;
    std::string curComboName;
    SoundEntry cur;
    std::vector<int> rawDelay, rawVol;
    std::vector<std::pair<PoolSpec*, int>> plainOrder;

    auto finishEntry = [&]() {
        if (!inAttack) return;
        for (size_t i = 0; i < plainOrder.size(); ++i) {
            SoundSpec& sp = plainOrder[i].first->specs[plainOrder[i].second];
            if (i < rawDelay.size() && rawDelay[i] > 0) sp.delay = rawDelay[i];
            if (i < rawVol.size()) sp.vol = ClampInt(rawVol[i], 0, 200);   // 0..200
        }
        cur.combo = (inCombo && curComboW >= 0) ? curComboName : "";
        // 修复历史保存 bug：默认组合的条目以前没写 [WeaponW] 段头，会掉进上一个
        // 命名组合的段里（例如长枪条目跑到 [Weapon3:DIO] 里 → 变成"武器6的组合DIO"）。
        // 段里的组合武器和自己 WeaponType 不一致 → 判为错位，按默认组合处理。
        // （任意武器条目 WeaponType=-1 也一样：组合只对具体武器有意义，它只属于默认组合）
        if (inCombo && curComboW >= 0 && cur.weaponType != curComboW) {
            cur.combo.clear();
            ++cfg.fixedCombos;
        }
        cfg.entries.push_back(std::move(cur));
        inAttack = false;
        cur = SoundEntry();
        rawDelay.clear();
        rawVol.clear();
        plainOrder.clear();
    };

    size_t pos = 0;
    while (pos < txt.size()) {
        size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = Trim(txt.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[' && line.back() == ']') {
            finishEntry();
            section = Trim(line.substr(1, line.size() - 2));
            if (section.size() >= 6 && section.compare(0, 6, "Attack") == 0) {
                inAttack = true;
                cur = SoundEntry();
            } else if (section == "Active") {
                inAttack = false; inCombo = false; curComboW = -1; curComboName.clear();
            } else if (section == "Combos") {
                inAttack = false; inCombo = false; curComboW = -1; curComboName.clear();
            } else if (section.size() > 6 && section.compare(0, 6, "Weapon") == 0) {
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
                inCombo = false; curComboW = -1; curComboName.clear(); inAttack = false;
            }
            continue;
        }

        // 找键值分隔的 '='。不能用第一个——条件表达式里的比较符(>= <= == !=)
        // 也含 '='，比如  Sound:dmg>0 & dAura>=0 = a.wav  按第一个切会把键截断。
        size_t eq = std::string::npos;
        for (size_t i2 = 0; i2 < line.size(); ++i2) {
            if (line[i2] != '=') continue;
            const char pv = (i2 > 0) ? line[i2 - 1] : (char)0;
            const char nx = (i2 + 1 < line.size()) ? line[i2 + 1] : (char)0;
            if (pv == '>' || pv == '<' || pv == '!' || pv == '=') continue;
            if (nx == '=') continue;
            eq = i2; break;
        }
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));

        if (!inAttack) {
            if (section == "WeaponSoundEnhance") {
                if (key == "PlayerRoot") cfg.global.playerRoot = val;
                else if (key == "PollMs") cfg.global.pollMs = std::atoi(val.c_str());
                else if (key == "DebounceMs") cfg.global.debounceMs = std::atoi(val.c_str());
                else if (key == "Volume") cfg.global.volume = std::atoi(val.c_str());
                else if (key == "Enabled") cfg.global.enabled = std::atoi(val.c_str());
                else if (key == "MoreSounds") cfg.global.moreSounds = std::atoi(val.c_str());
        else if (key == "EfxCapture")  cfg.global.efxCapture  = std::atoi(val.c_str());
        else if (key == "EfxLogAll")   cfg.global.efxLogAll   = std::atoi(val.c_str());
        else if (key == "EfxDiscover") cfg.global.efxDiscover = std::atoi(val.c_str());
                else if (key == "Debug") cfg.global.debug = std::atoi(val.c_str());
                else if (key == "GaugePtrOff") cfg.global.gaugePtrOff = val;
                else if (key == "GaugeValOff") cfg.global.gaugeValOff = val;
                else if (key == "ChargeValOff") cfg.global.chargeValOff = val;
                else if (key == "FsmTargetOff") cfg.global.fsmTargetOff = val;
                else if (key == "QuestRoot") cfg.global.questRoot = val;
                else if (key == "QuestDmgOff") cfg.global.questDmgOff = val;
                else if (key == "ChatEcho") cfg.global.chatEcho = std::atoi(val.c_str());
        else if (key == "UpdateCheck") cfg.global.updateCheck = std::atoi(val.c_str());
        else if (key == "UpdateProxy") cfg.global.updateProxy = val;
                else if (key == "ChatCommands") cfg.global.chatCommands = std::atoi(val.c_str());
                else if (key == "Hotkeys") cfg.global.hotkeysEnabled = std::atoi(val.c_str());
                else if (key == "WeaponEnabled") cfg.global.weaponEnabled = val;
                else if (key == "WemOnlyInQuest") cfg.global.wemOnlyInQuest = std::atoi(val.c_str());
                else if (key == "WemMapWhite") cfg.global.wemMapWhite = val;
                else if (key == "WemMapBlack") cfg.global.wemMapBlack = val;
                else if (key == "BankOutDir") cfg.global.bankOutDir = val;
            } else if (section == "Hotkeys") {
                if (key == "ModifierKey") cfg.hotkeys.modifierKey = std::atoi(val.c_str());
                else if (key == "ReloadKey") cfg.hotkeys.reloadKey = std::atoi(val.c_str());
                else if (key == "VolUpKey") cfg.hotkeys.volUpKey = std::atoi(val.c_str());
                else if (key == "VolDownKey") cfg.hotkeys.volDownKey = std::atoi(val.c_str());
                else if (key == "SetVolKey") cfg.hotkeys.setVolKey = std::atoi(val.c_str());
                else if (key == "SetVolValue") cfg.hotkeys.setVolValue = std::atoi(val.c_str());
                else if (key == "ToggleKey") cfg.hotkeys.toggleKey = std::atoi(val.c_str());
                else if (key == "MoreKey") cfg.hotkeys.moreKey = std::atoi(val.c_str());
                else if (key == "ComboKey") cfg.hotkeys.comboKey = std::atoi(val.c_str());
            } else if (section == "Active") {
                if (key.size() > 1 && (key[0] == 'W' || key[0] == 'w')) {
                    int w = std::atoi(key.c_str() + 1);
                    if (w >= -1) activeMap[w] = val;
                }
            } else if (section == "Combos") {
                // [Combos] W<type>=组合1;组合2 —— 每武器组合名列表（含空组合），
                // 分开存好，条目推导不能代替它。
                if (key.size() > 1 && (key[0] == 'W' || key[0] == 'w')) {
                    int w = std::atoi(key.c_str() + 1);
                    if (w < 0 || w > 13) continue;
                    for (const auto& raw : SplitList(val)) {
                        std::string nm = Trim(raw);
                        if (nm.empty()) continue;
                        bool has = false;
                        for (const auto& x : cfg.comboList[w]) if (x == nm) { has = true; break; }
                        if (!has) cfg.comboList[w].push_back(nm);
                    }
                }
            }
            continue;
        }

        if (key == "WeaponType") {
            cur.weaponType = std::atoi(val.c_str());
        } else if (key == "Media") {
            // WWise wem/media id（wem 捕获条目）：-1/0 = 不按 media 判定
            cur.media = (long long)std::strtoll(val.c_str(), nullptr, 10);
        } else if (key == "Bank") {
            cur.bank = val;
        } else if (key == "FSMId") {
            cur.fsmId = std::atoi(val.c_str());
        } else if (key == "ActionLMT" || key == "LMT") {
            // 严格解析：只认整数；-1 / any / all / * / 不限 = 不限（清空列表）
            // （以前用 atoi，手写成 "49265-1" 会被静默截断成 49265）
            for (const auto& t0 : SplitList(val)) {
                std::string t = Trim(t0);
                std::string low = t;
                for (auto& c : low) if (c >= 'A' && c <= 'Z') c += 32;
                if (low == "-1" || low == "any" || low == "all" || low == "*" || low == "不限") {
                    cur.lmt.clear();
                    break;
                }
                bool digits = !t.empty();
                size_t st = (t[0] == '+' || t[0] == '-') ? 1 : 0;
                if (st >= t.size()) digits = false;
                for (size_t k = st; k < t.size() && digits; ++k)
                    if (t[k] < '0' || t[k] > '9') digits = false;
                if (!digits) continue;                  // 无法识别 → 忽略（GUI 会提示）
                const int v = std::atoi(t.c_str());
                if (v < 0) continue;
                bool dup = false;
                for (int x : cur.lmt) if (x == v) { dup = true; break; }
                if (!dup) cur.lmt.push_back(v);
            }
        } else if (key == "Name") {
            cur.name = val;
        } else if (key == "Stop") {
            cur.stop = std::atoi(val.c_str()) != 0;
        } else if (key == "CheckDelayMs") {
            cur.checkDelayMs = std::atoi(val.c_str());
        } else if (key == "CheckTimeoutMs") {
            cur.checkTimeoutMs = std::atoi(val.c_str());
        } else if (key == "CheckOffsetMs" || key == "CheckGraceMs") {
            cur.checkOffsetMs = std::atoi(val.c_str());
            if (cur.checkOffsetMs < 0) cur.checkOffsetMs = 0;
        } else if (key == "CheckEndOn") {
            std::string lv;
            for (char c : val) lv += (char)tolower((unsigned char)c);
            cur.endOnAction = (lv == "action");
        } else if (key == "CheckMode") {
            std::string lv;
            for (char c : val) lv += (char)tolower((unsigned char)c);
            cur.checkMode = (lv == "final") ? 1 : 0;
        } else if (key == "JudgePreset") {
            cur.judgePreset = std::atoi(val.c_str());
        } else if (key == "FSMTarget") {
            cur.fsmTarget = std::atoi(val.c_str());
        } else if (key == "Group") {
            cur.group = val;
        } else if (key == "Sound") {
            AppendSpecs(cur.def, val, plainOrder);
        } else if ((key.size() > 6 && key.compare(0, 6, "Sound:") == 0 &&
                    key.find_first_of("<>=!&|", 6) != std::string::npos) ||
                   (key.size() > 9 && key.compare(0, 9, "SoundEnd:") == 0)) {
            // 条件池：Sound:<表达式>= 或 SoundEnd:<表达式>=
            // 判据是"标签里含比较符"，不含的仍按旧的刃时 tag 解析，老配置零影响。
            const bool atEnd = (key.size() > 9 && key.compare(0, 9, "SoundEnd:") == 0);
            CondSpec cs;
            cs.expr = Trim(key.substr(atEnd ? 9 : 6));
            cs.atEnd = atEnd;
            std::vector<std::pair<PoolSpec*, int>> scratch;
            AppendSpecs(cs.pool, val, scratch);
            cur.conds.push_back(cs);
        } else if (key.size() > 6 && key.compare(0, 6, "Sound:") == 0) {
            int lvl = GaugeTagIndex(ToLower(key.substr(6)));
            if (lvl >= 0 && lvl < 4)
                AppendSpecs(cur.gauge[lvl], val, plainOrder);
        } else if (key == "SoundDelay") {
            for (const auto& t : SplitList(val))
                rawDelay.push_back(std::atoi(t.c_str()));
        } else if (key == "SoundVol") {
            for (const auto& t : SplitList(val))
                rawVol.push_back(std::atoi(t.c_str()));
        }
    }
    finishEntry();

    // [Active] 里指向"该武器并不存在的组合"的映射一律丢掉（历史 bug 会留下 W6=DIO 这种
    // 从别的武器串过来的名字；留着的话新条目会被塞进这个幽灵组合）。
    for (auto it = activeMap.begin(); it != activeMap.end();) {
        if (it->second.empty() || it->first < 0) { ++it; continue; }
        bool exists = false;
        for (const auto& e : cfg.entries)
            if (e.weaponType == it->first && e.combo == it->second) { exists = true; break; }
        if (!exists) it = activeMap.erase(it);
        else ++it;
    }
    cfg.active = activeMap;
    cfg.path = path;
    cfg.loaded = true;
    return true;
}

// 一条音效的规范写法：默认属性且未固定/无冷却 → 纯路径；否则 path|delay|vol[|F][|C<ms>][|P]
static std::string SpecToken(const SoundSpec& s) {
    if (!s.fixed && s.delay == 0 && s.vol == 100 && s.cdMs == 0 && !s.playLock) return s.path;
    std::string t = s.path + "|" + std::to_string(s.delay) + "|" + std::to_string(s.vol);
    std::string flags;
    if (s.fixed) flags += "F";
    if (s.cdMs > 0) flags += "C" + std::to_string(s.cdMs);
    if (s.playLock) flags += "P";
    if (!flags.empty()) t += "|" + flags;
    return t;
}

static std::string LmtLine(const SoundEntry& e) {
    if (e.lmt.empty()) return "ActionLMT=-1";          // v1 兼容写法
    if (e.lmt.size() == 1) return "ActionLMT=" + std::to_string(e.lmt[0]);
    std::string s = "LMT=";
    for (size_t k = 0; k < e.lmt.size(); ++k) {
        if (k) s += ",";
        s += std::to_string(e.lmt[k]);
    }
    return s;
}

static void WritePool(const PoolSpec& pool, const char* key, std::string& o) {
    if (pool.empty()) return;
    std::string s;
    for (size_t k = 0; k < pool.specs.size(); ++k) {
        if (k) s += "; ";
        s += SpecToken(pool.specs[k]);
    }
    o += std::string(key) + "=" + s + "\r\n";
}

static void WriteEntry(const SoundEntry& e, int n, std::string& o) {
    o += "[Attack" + std::to_string(n) + "]\r\n";
    if (!e.name.empty()) o += "Name=" + e.name + "\r\n";
    if (!e.group.empty()) o += "Group=" + e.group + "\r\n";
    if (e.stop) o += "Stop=1\r\n";
    o += "WeaponType=" + std::to_string(e.weaponType) + "\r\n";
    o += LmtLine(e) + "\r\n";
    o += "FSMId=" + std::to_string(e.fsmId) + "\r\n";
    if (e.media >= 0) o += "Media=" + std::to_string(e.media) + "\r\n";
    if (!e.bank.empty()) o += "Bank=" + e.bank + "\r\n";
    if (e.fsmTarget >= 0) o += "FSMTarget=" + std::to_string(e.fsmTarget) + "\r\n";
    if (e.checkTimeoutMs > 0 && !e.conds.empty()) {
        if (e.checkDelayMs > 0)
            o += "CheckDelayMs=" + std::to_string(e.checkDelayMs) + "\r\n";
        o += "CheckTimeoutMs=" + std::to_string(e.checkTimeoutMs) + "\r\n";
        if (e.endOnAction) o += "CheckEndOn=action\r\n";
        if (e.checkMode) o += "CheckMode=final\r\n";
        if (e.judgePreset) o += "JudgePreset=" + std::to_string(e.judgePreset) + "\r\n";
        o += "CheckOffsetMs=" + std::to_string(e.checkOffsetMs) + "\r\n";
        for (const auto& c : e.conds) {
            if (c.pool.empty() || c.expr.empty()) continue;
            const std::string k = (c.atEnd ? "SoundEnd:" : "Sound:") + c.expr;
            WritePool(c.pool, k.c_str(), o);
        }
    }
    WritePool(e.def, "Sound", o);
    for (int i = 0; i < 4; ++i) {
        std::string k = std::string("Sound:") + GaugeTagName(i);
        WritePool(e.gauge[i], k.c_str(), o);
    }
    o += "\r\n";
}

bool SaveConfig(const std::string& path, const Config& cfg) {
    std::string o;
    o += "; ============================================================================\r\n";
    o += ";  WeaponSoundEnhance.ini —— 武器音效拓展插件配置\r\n";
    o += ";  (由 Sonar GUI 生成)\r\n";
    o += ";  路径：本文件在 plugins\\WeaponSoundEnhance\\ 下（与 DLL 同目录的旧布局也兼容）；\r\n";
    o += ";        音效 wav 放在本文件同级的 sounds\\ 文件夹里（旧位置 plugins\\sounds\\ 仍自动兼容）。\r\n";
    o += ";\r\n";
    o += ";  武器类型表（与原版武器序号一致）：\r\n";
    o += ";    0=大剑  1=片手  2=双刀  3=太刀  4=大锤  5=笛子\r\n";
    o += ";    6=长枪  7=铳枪  8=斩斧  9=盾斧 10=虫棍 11=弓箭 12=重弩 13=轻弩\r\n";
    o += ";  每条 [AttackN] 代表一种派生攻击：\r\n";
    o += ";    WeaponType  ：武器类型（0..13）。-1 = 任意武器。\r\n";
    o += ";    ActionLMT/LMT：动作 LMT。不限 = 空 / -1 / any / * / 不限（该 FSMId 的所有动作都触发）；\r\n";
    o += ";                   多个用逗号分隔（LMT=49265,49256）。只接受整数。\r\n";
    o += ";    FSMId       ：动作状态机 ID。-1 = 不限。\r\n";
    o += ";    三者 AND 关系；命中后从匹配的音效里选音效。\r\n";
    o += ";    Group=              动作组（可选）：同一招的多个触发条目填相同组名\r\n";
    o += ";                        时整招只响一次，刃色以首个触发瞬间为准。\r\n";
    o += ";    Sound=               默认音效；Sound:none/white/yellow/red= 太刀无刃时/白刃时/\r\n";
    o += ";                        黄刃时/红刃时 的音效（未配的刃时回退默认音效）。\r\n";
    o += ";    音效写法 path|延时ms|音量0..200|F ，末尾 F = 固定音效：命中时恒播，\r\n";
    o += ";                        同池其余未固定音效仍随机抽一条同时播放。\r\n";
    o += ";  每武器配置组合：默认组合的条目写在 [WeaponW] 段里；命名组合写在\r\n";
    o += ";    [WeaponW:组合名] 段里；[Active] 段 W<type>=组合名 决定每武器当前用哪个组合。\r\n";
    o += ";  游戏内聊天框指令：/wse reload | on | off | more | one | vol N | vol+ | vol- | "
         "combo [名] | combos | help\r\n";
    o += "; ============================================================================\r\n\r\n";

    o += "[WeaponSoundEnhance]\r\n";
    o += "PlayerRoot=" + cfg.global.playerRoot + "\r\n";
    o += "PollMs=" + std::to_string(cfg.global.pollMs) + "\r\n";
    o += "DebounceMs=" + std::to_string(cfg.global.debounceMs) + "\r\n";
    o += "Volume=" + std::to_string(cfg.global.volume) + "\r\n";
    o += "Enabled=" + std::to_string(cfg.global.enabled) + "\r\n";
    o += "MoreSounds=" + std::to_string(cfg.global.moreSounds) + "\r\n";
    o += "; 特效捕获（实验性）：0 = 关闭捕获；EfxLogAll / EfxDiscover 是诊断开关（默认 1）\r\n";
    o += "EfxCapture=" + std::to_string(cfg.global.efxCapture) + "\r\n";
    o += "EfxLogAll=" + std::to_string(cfg.global.efxLogAll) + "\r\n";
    o += "EfxDiscover=" + std::to_string(cfg.global.efxDiscover) + "\r\n";
    o += "Debug=" + std::to_string(cfg.global.debug) + "\r\n";
    o += "GaugePtrOff=" + cfg.global.gaugePtrOff + "\r\n";
    o += "GaugeValOff=" + cfg.global.gaugeValOff + "\r\n";
    o += "ChargeValOff=" + cfg.global.chargeValOff + "\r\n";
    o += "FsmTargetOff=" + cfg.global.fsmTargetOff + "\r\n";
    o += "QuestRoot=" + cfg.global.questRoot + "\r\n";
    o += "QuestDmgOff=" + cfg.global.questDmgOff + "\r\n";
    o += "ChatEcho=" + std::to_string(cfg.global.chatEcho) + "\r\n";
    if (cfg.global.updateCheck != 1) o += "UpdateCheck=" + std::to_string(cfg.global.updateCheck) + "\r\n";
    if (!cfg.global.updateProxy.empty()) o += "UpdateProxy=" + cfg.global.updateProxy + "\r\n";
    o += "ChatCommands=" + std::to_string(cfg.global.chatCommands) + "\r\n";
    o += "Hotkeys=" + std::to_string(cfg.global.hotkeysEnabled) + "\r\n";
    // 武器触发开关 / wem 场景过滤（一定要随保存输出，否则一保存就丢）
    if (!cfg.global.weaponEnabled.empty()) o += "WeaponEnabled=" + cfg.global.weaponEnabled + "\r\n";
    if (cfg.global.wemOnlyInQuest != 0)     o += "WemOnlyInQuest=" + std::to_string(cfg.global.wemOnlyInQuest) + "\r\n";
    if (!cfg.global.wemMapWhite.empty())    o += "WemMapWhite=" + cfg.global.wemMapWhite + "\r\n";
    if (!cfg.global.wemMapBlack.empty())    o += "WemMapBlack=" + cfg.global.wemMapBlack + "\r\n";
    if (!cfg.global.bankOutDir.empty())     o += "BankOutDir=" + cfg.global.bankOutDir + "\r\n\r\n";

    o += "[Hotkeys]\r\n";
    o += "ModifierKey=" + std::to_string(cfg.hotkeys.modifierKey) + "\r\n";
    o += "ReloadKey=" + std::to_string(cfg.hotkeys.reloadKey) + "\r\n";
    o += "VolUpKey=" + std::to_string(cfg.hotkeys.volUpKey) + "\r\n";
    o += "VolDownKey=" + std::to_string(cfg.hotkeys.volDownKey) + "\r\n";
    o += "SetVolKey=" + std::to_string(cfg.hotkeys.setVolKey) + "\r\n";
    o += "SetVolValue=" + std::to_string(cfg.hotkeys.setVolValue) + "\r\n";
    o += "ToggleKey=" + std::to_string(cfg.hotkeys.toggleKey) + "\r\n";
    o += "MoreKey=" + std::to_string(cfg.hotkeys.moreKey) + "\r\n";
    o += "ComboKey=" + std::to_string(cfg.hotkeys.comboKey) + "\r\n";

    // [Active]：记录每武器当前用的组合（仅对有组合的武器写出）
    {
        std::string act;
        for (int w = 0; w <= 13; ++w) {
            bool any = false, hasNamed = false;
            for (const auto& e : cfg.entries) if (e.weaponType == w) { any = true; if (!e.combo.empty()) hasNamed = true; }
            if (!any) continue;
            std::string a;
            auto it = cfg.active.find(w);
            if (it != cfg.active.end()) a = it->second;
            if (hasNamed || !a.empty())
                act += std::string("W") + std::to_string(w) + "=" + a + "\r\n";
        }
        if (!act.empty()) o += "\r\n[Active]\r\n" + act + "\r\n";
    }

    // [Combos]：每武器组合名列表（含空组合，顺序=显示顺序）。
    // 加载时 merge 进 cfg.comboList；条目里出现但列表没有的（老配置）也一并补录，
    // 保证升级后旧组合名不丢、顺序为"列表在前 + 新增在后"。
    {
        std::string blk;
        for (int w = 0; w <= 13; ++w) {
            std::vector<std::string> names;
            if (cfg.comboList.count(w)) names = cfg.comboList.at(w);
            for (const auto& e : cfg.entries) {
                if (e.weaponType != w || e.combo.empty()) continue;
                bool has = false;
                for (const auto& x : names) if (x == e.combo) { has = true; break; }
                if (!has) names.push_back(e.combo);
            }
            if (names.empty()) continue;
            std::string line = "W" + std::to_string(w) + "=";
            for (size_t i = 0; i < names.size(); ++i) {
                if (i) line += ";";
                line += names[i];
            }
            blk += line + "\r\n";
        }
        if (!blk.empty()) o += "\r\n[Combos]\r\n" + blk + "\r\n";
    }

    // 按武器 + 组合分组写出（段名 AttackN 全局递增）
    int n = 0;
    auto Banner = [&](const std::string& wname, int w, bool& header) {
        if (!header) {
            o += "\r\n; ----------------------------------------------------------------------------\r\n";
            o += std::string("; ") + wname + " (weaponType=" + std::to_string(w) + ")\r\n";
            o += "; ----------------------------------------------------------------------------\r\n";
            header = true;
        }
    };
    auto Has = [&](const std::vector<std::string>& v, const std::string& e) {
        for (const auto& x : v) if (x == e) return true;
        return false;
    };

    for (int w = 0; w <= 13; ++w) {
        bool header = false;
        // 默认组合（""）：必须写一个 [WeaponW] 段头把"组合上下文"清掉！
        // 以前不写段头，默认组合的条目会掉进上一个命名组合的段里 —— 保存再读取就
        // 变成"默认组合空了、条目跑到别的组合（甚至别的武器）里去了"。
        bool anyDefault = false;
        for (const auto& e : cfg.entries)
            if (e.weaponType == w && e.combo.empty()) { anyDefault = true; break; }
        if (anyDefault) {
            Banner(WeaponName(w), w, header);
            o += "\r\n[Weapon" + std::to_string(w) + "]\r\n";
            for (const auto& e : cfg.entries)
                if (e.weaponType == w && e.combo.empty()) WriteEntry(e, ++n, o);
        }
        // 命名组合（按条目中出现顺序）
        std::vector<std::string> cbOrder;
        for (const auto& e : cfg.entries)
            if (e.weaponType == w && !e.combo.empty() && !Has(cbOrder, e.combo)) cbOrder.push_back(e.combo);
        for (const auto& cb : cbOrder) {
            Banner(WeaponName(w), w, header);
            o += "\r\n[Weapon" + std::to_string(w) + ":" + cb + "]\r\n";
            for (const auto& e : cfg.entries)
                if (e.weaponType == w && e.combo == cb) WriteEntry(e, ++n, o);
        }
    }
    // 任意武器（weaponType<0）固定属于默认组合（组合只对具体武器有意义），放最后
    {
        bool any = false;
        for (const auto& e : cfg.entries) if (e.weaponType < 0) { any = true; break; }
        if (any) {
            o += "\r\n; ----------------------------------------------------------------------------\r\n";
            o += "; 通用 / 任意武器\r\n";
            o += "; ----------------------------------------------------------------------------\r\n";
            o += "\r\n[WeaponAny]\r\n";
            for (const auto& e : cfg.entries)
                if (e.weaponType < 0) WriteEntry(e, ++n, o);
        }
    }

    // 宽路径写盘（同读取：中文路径下 std::ofstream 会写到不存在的位置/直接失败）
    return FsWrite(path, o);
}

// 组合段名：[WeaponW]（默认组合） / [WeaponW:组合名]
static std::string GetComboSectionName(int w, const std::string& combo) {
    if (combo.empty()) return "Weapon" + std::to_string(w);
    return "Weapon" + std::to_string(w) + ":" + combo;
}

// 导出单个组合为可分享的 ini 片段：
//   文件开头有说明头；然后是 [WeaponW:组合名] 段 + 该组合的全部条目。
//   导入方用 LoadConfig 直接解析，再按条目去重合并（和「合并旧版ini」同一套逻辑）。
bool BuildComboExportText(int weapon, const std::string& combo,
                          const std::vector<SoundEntry>& all,
                          std::string& outText, std::string& err) {
    // 导出文件带 UTF-8 BOM：记事本/其它编辑器打开中文不乱码（导入端解析会跳过 BOM）
    outText.clear();
    outText = "\xEF\xBB\xBF";
    outText += "; ============================================================================\r\n";
    outText += ";  Sonar 组合导出 v1\r\n";
    outText += ";  武器: " + std::string(WeaponName(weapon)) + " (" + std::to_string(weapon) + ")\r\n";
    outText += ";  组合: " + (combo.empty() ? "默认" : combo) + "\r\n";
    outText += ";  导入：GUI 工具栏「导入组合」选择本文件（或它对应的 .zip），按条目去重合并。\r\n";
    outText += "; ============================================================================\r\n\r\n";
    outText += "[" + GetComboSectionName(weapon, combo) + "]\r\n";

    int n = 0, written = 0;
    for (const auto& e : all) {
        if (e.weaponType != weapon || e.combo != combo) continue;
        WriteEntry(e, ++n, outText);
        ++written;
    }
    if (written == 0) { err = "这个组合一条条目都没有，没什么可导出的"; return false; }
    err.clear();
    return true;
}

bool ExportComboFile(const std::string& path, int weapon, const std::string& combo,
                     const std::vector<SoundEntry>& all, std::string& err) {
    std::string txt;
    if (!BuildComboExportText(weapon, combo, all, txt, err)) return false;
    if (!FsWrite(path, txt)) { err = "写文件失败: " + path; return false; }
    return true;
}
