// ===========================================================================
//  core.cpp —— Sonar GUI 的"无界面业务核心"
//
//  这里是从旧版 app.cpp（Dear ImGui，3400 行）里把**界面之外**的一切搬过来的：
//  配置读写 / 条目增删改 / 配置组合 / 导入导出组合包 / FSM-LMT 共享库 /
//  实时捕获 / 在线更新 / 界面偏好持久化。
//
//  ★ 一条铁律：本文件不认识任何界面。对外只有 Core::Handle(method, json) 和
//    Core::PollEvents()，宿主（webview_host.cpp）把 JSON 原样转给 JS 就行。
//    加功能不用改宿主，加方法不用改头文件以外的任何东西。
//
//  ★ 为了不引入 JSON 第三方库（工程里一个都没有，也不想为一个 GUI 拉一个新依赖），
//    下面自带了一个"够用就好"的 JSON 读写：writer 保证输出**原始 UTF-8**
//    （中文绝不转 \uXXXX —— WebView2 侧按 UTF-8 解析，转义只会让界面显示乱码），
//    parser 支持对象/数组/字符串(含转义)/数字/true/false/null，够解析前端传参。
//
//  ★ 关于 include 顺序：miniz.c 会定义一批短名字宏（R/G/B/A 之类），所以要在
//    所有 windows 头之后、并且单独用 pragma 把它的警告压掉。只有本文件 include
//    miniz.c —— 新的 GUI 进程不含 app.cpp，不会重复符号。
// ===========================================================================

#include "core.h"
#include "app.h"        // kWseGuiVersion（版本号只此一处，避免两份对不上）
#include "bankmod.h"    // nbnk 音效替换（Mod 制作）
#include "config.h"
#include "fsmdb.h"
#include "fsutil.h"
#include "game.h"
#include "update.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>     // SHBrowseForFolder（选导出目录）
#include <shellapi.h>
#include <mmsystem.h>
#include <urlmon.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// 音效解码（wav/mp3/ogg/flac -> 16-bit PCM）：试听用。
// 放在 miniz 之前 include —— miniz 的宏污染不到它内部的解码器。
#define WSE_AUDIO_IMPLEMENTATION
#include "wse_audio.h"

// zip 打包/解包（组合包导入导出；miniz 单文件公有领域）
#pragma warning(push)
#pragma warning(disable: 4100 4201 4242 4244 4245 4267 4305 4324 4700 4701 4702 4706 4996 6011 6262 6387)
#include "miniz.c"
#pragma warning(pop)

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "urlmon.lib")

// ===========================================================================
//  一、小 JSON 工具（writer + parser）
// ===========================================================================
namespace {

// 对象用 map：键少（前端传的参数最多十几个），查找是线性的也无所谓，
// 好处是序列化顺序稳定（map 有序）—— 调试时输出的 JSON 前后一致，方便比对。
struct JVal;
using JObj = std::map<std::string, JVal>;
using JArr = std::vector<JVal>;

struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    Type t = NUL;
    bool b = false;
    double n = 0;
    bool integral = true;   // 整数值：输出时不带小数点（前端拿到的 3 而不是 3.0）
    std::string s;
    JArr a;
    JObj o;

    JVal() {}
    JVal(bool v) : t(BOOL), b(v) {}
    JVal(int v) : t(NUM), n((double)v) {}
    JVal(long long v) : t(NUM), n((double)v) {}
    JVal(double v) : t(NUM), n(v), integral(false) {}
    JVal(const char* v) : t(STR), s(v ? v : "") {}
    JVal(const std::string& v) : t(STR), s(v) {}

    static JVal obj() { JVal v; v.t = OBJ; return v; }
    static JVal arr() { JVal v; v.t = ARR; return v; }
    static JVal null() { return JVal(); }

    void set(const std::string& k, JVal v) { t = OBJ; o[k] = std::move(v); }
    void push(JVal v) { t = ARR; a.push_back(std::move(v)); }

    bool isNum() const { return t == NUM; }
    bool isStr() const { return t == STR; }
    bool isObj() const { return t == OBJ; }
    bool isArr() const { return t == ARR; }
    bool isBool() const { return t == BOOL; }
    bool isNull() const { return t == NUL; }

    // ---- 取值一律给默认值：前端少传字段是常态，不能因此判错 ----
    int asInt(int def = 0) const {
        if (t == NUM) return (int)(n < 0 ? (n - 0.5) : (n + 0.5));   // 四舍五入，避免 2.999 -> 2
        if (t == BOOL) return b ? 1 : 0;
        if (t == STR) return std::atoi(s.c_str());
        return def;
    }
    bool asBool(bool def = false) const {
        if (t == BOOL) return b;
        if (t == NUM) return n != 0;
        return def;
    }
    std::string asStr(const std::string& def = std::string()) const {
        if (t == STR) return s;
        if (t == NUM) {
            char b2[40];
            if (integral) snprintf(b2, sizeof(b2), "%lld", (long long)n);
            else snprintf(b2, sizeof(b2), "%g", n);
            return std::string(b2);
        }
        return def;
    }
    // 取子对象/子数组；类型不对返回 nullptr（调用方按"没传"处理）
    const JVal* find(const std::string& k) const {
        if (t != OBJ) return nullptr;
        JObj::const_iterator it = o.find(k);
        return (it == o.end()) ? nullptr : &it->second;
    }
    std::string optStr(const std::string& k, const std::string& def = std::string()) const {
        const JVal* v = find(k);
        return v ? v->asStr(def) : def;
    }
    int optInt(const std::string& k, int def) const {
        const JVal* v = find(k);
        return v ? v->asInt(def) : def;
    }
    bool optBool(const std::string& k, bool def) const {
        const JVal* v = find(k);
        return v ? v->asBool(def) : def;
    }
    bool has(const std::string& k) const { return find(k) != nullptr; }
};

// ---------------------------------------------------------------------------
//  转义：只转 JSON 规定的字符。**高位字节原样输出**（这就是"原始 UTF-8"），
//  控制字符按 \u00XX 转 —— 音效路径/名称里偶尔混进 \t \r 也不会毁掉整个 JSON。
// ---------------------------------------------------------------------------
void JsonEscape(const std::string& s, std::string& out) {
    out += '"';
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", (unsigned)c);
                    out += b;
                } else {
                    out += (char)c;
                }
                break;
        }
    }
    out += '"';
}

void JsonWrite(const JVal& v, std::string& out) {
    switch (v.t) {
        case JVal::NUL: out += "null"; break;
        case JVal::BOOL: out += v.b ? "true" : "false"; break;
        case JVal::NUM: {
            char b[40];
            if (v.integral) snprintf(b, sizeof(b), "%lld", (long long)v.n);
            else            snprintf(b, sizeof(b), "%.6g", v.n);
            out += b;
            break;
        }
        case JVal::STR: JsonEscape(v.s, out); break;
        case JVal::ARR: {
            out += '[';
            for (std::size_t i = 0; i < v.a.size(); ++i) {
                if (i) out += ',';
                JsonWrite(v.a[i], out);
            }
            out += ']';
            break;
        }
        case JVal::OBJ: {
            out += '{';
            bool first = true;
            for (JObj::const_iterator it = v.o.begin(); it != v.o.end(); ++it) {
                if (!first) out += ',';
                first = false;
                JsonEscape(it->first, out);
                out += ':';
                JsonWrite(it->second, out);
            }
            out += '}';
            break;
        }
    }
}

std::string JsonDump(const JVal& v) {
    std::string out;
    out.reserve(4096);
    JsonWrite(v, out);
    return out;
}

// ---------------------------------------------------------------------------
//  解析器：递归下降。只求"能正确解析前端传参"，不追求报错信息好用。
//  字符串里的 \uXXXX 要处理：前端 JSON.stringify 出来的中文虽然是原样 UTF-8，
//  但用户手写/别的工具生成的参数可能是转义的，得能吃下去。
// ---------------------------------------------------------------------------
class JsonParser {
public:
    explicit JsonParser(const std::string& s) : mS(s) {
        // 前端可能带 BOM 的文件读出来；这里对传参也一样容错
        if (mS.size() >= 3 && (unsigned char)mS[0] == 0xEF &&
            (unsigned char)mS[1] == 0xBB && (unsigned char)mS[2] == 0xBF)
            mI = 3;
    }

    bool parse(JVal& out) {
        skipWs();
        if (!value(out)) return false;
        skipWs();
        return mI >= mS.size();   // 尾部还有垃圾 = 解析失败（宁可判错也别猜）
    }

private:
    const std::string& mS;
    std::size_t mI = 0;

    void skipWs() {
        while (mI < mS.size()) {
            const char c = mS[mI];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++mI;
            else break;
        }
    }
    bool lit(const char* w) {
        const std::size_t n = strlen(w);
        if (mS.compare(mI, n, w) != 0) return false;
        mI += n;
        return true;
    }
    bool value(JVal& out) {
        if (mI >= mS.size()) return false;
        const char c = mS[mI];
        if (c == '{') return obj(out);
        if (c == '[') return arr(out);
        if (c == '"') { std::string s; if (!str(s)) return false; out = JVal(s); return true; }
        if (lit("true"))  { out = JVal(true);  return true; }
        if (lit("false")) { out = JVal(false); return true; }
        if (lit("null"))  { out = JVal();      return true; }
        return num(out);
    }
    bool obj(JVal& out) {
        out = JVal::obj();
        ++mI;   // '{'
        skipWs();
        if (mI < mS.size() && mS[mI] == '}') { ++mI; return true; }
        for (;;) {
            skipWs();
            std::string k;
            if (!str(k)) return false;
            skipWs();
            if (mI >= mS.size() || mS[mI] != ':') return false;
            ++mI;
            skipWs();
            JVal v;
            if (!value(v)) return false;
            out.o[k] = std::move(v);
            skipWs();
            if (mI >= mS.size()) return false;
            if (mS[mI] == ',') { ++mI; continue; }
            if (mS[mI] == '}') { ++mI; return true; }
            return false;
        }
    }
    bool arr(JVal& out) {
        out = JVal::arr();
        ++mI;   // '['
        skipWs();
        if (mI < mS.size() && mS[mI] == ']') { ++mI; return true; }
        for (;;) {
            skipWs();
            JVal v;
            if (!value(v)) return false;
            out.a.push_back(std::move(v));
            skipWs();
            if (mI >= mS.size()) return false;
            if (mS[mI] == ',') { ++mI; continue; }
            if (mS[mI] == ']') { ++mI; return true; }
            return false;
        }
    }
    bool str(std::string& out) {
        if (mI >= mS.size() || mS[mI] != '"') return false;
        ++mI;
        out.clear();
        while (mI < mS.size()) {
            const char c = mS[mI++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (mI >= mS.size()) return false;
            const char e = mS[mI++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (mI + 4 > mS.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = mS[mI + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else return false;
                    }
                    mI += 4;
                    // UTF-16 代理对：\uD83D\uDE00 这类要拼成一个码点再编码
                    if (cp >= 0xD800 && cp <= 0xDBFF && mI + 6 <= mS.size() &&
                        mS[mI] == '\\' && mS[mI + 1] == 'u') {
                        unsigned lo = 0;
                        bool okLo = true;
                        for (int k = 0; k < 4; ++k) {
                            const char h = mS[mI + 2 + k];
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                            else { okLo = false; break; }
                        }
                        if (okLo && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            mI += 6;
                        }
                    }
                    Utf8Encode(cp, out);
                    break;
                }
                default: return false;
            }
        }
        return false;   // 没遇到收尾引号
    }
    bool num(JVal& out) {
        const std::size_t b = mI;
        if (mI < mS.size() && (mS[mI] == '-' || mS[mI] == '+')) ++mI;
        bool digits = false;
        while (mI < mS.size() && mS[mI] >= '0' && mS[mI] <= '9') { ++mI; digits = true; }
        if (mI < mS.size() && mS[mI] == '.') {
            ++mI;
            while (mI < mS.size() && mS[mI] >= '0' && mS[mI] <= '9') { ++mI; digits = true; }
        }
        if (!digits) return false;
        if (mI < mS.size() && (mS[mI] == 'e' || mS[mI] == 'E')) {
            ++mI;
            if (mI < mS.size() && (mS[mI] == '-' || mS[mI] == '+')) ++mI;
            while (mI < mS.size() && mS[mI] >= '0' && mS[mI] <= '9') ++mI;
        }
        const std::string t = mS.substr(b, mI - b);
        const bool isInt = t.find('.') == std::string::npos &&
                           t.find('e') == std::string::npos &&
                           t.find('E') == std::string::npos;
        JVal v;
        v.t = JVal::NUM;
        v.n = strtod(t.c_str(), nullptr);
        v.integral = isInt;
        out = v;
        return true;
    }
    static void Utf8Encode(unsigned cp, std::string& out) {
        if (cp <= 0x7F) {
            out += (char)cp;
        } else if (cp <= 0x7FF) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
};

// 解析失败时返回一个空对象：调用方拿不到任何参数，等价于"{}"，
// 拿默认值继续跑，不会因为前端传了个畸形 JSON 就整个方法失败。
JVal ParseParams(const std::string& json) {
    if (json.empty()) return JVal::obj();
    JsonParser p(json);
    JVal v;
    if (!p.parse(v)) return JVal::obj();
    if (!v.isObj()) return JVal::obj();
    return v;
}

// 统一信封：成功 {"ok":true,"data":...} / 失败 {"ok":false,"error":"..."}
std::string OkJson(const JVal& data) {
    JVal root = JVal::obj();
    root.set("ok", JVal(true));
    root.set("data", data);
    return JsonDump(root);
}
std::string OkJson() { return OkJson(JVal::obj()); }

std::string ErrJson(const std::string& msg) {
    JVal root = JVal::obj();
    root.set("ok", JVal(false));
    root.set("error", JVal(msg));
    return JsonDump(root);
}

} // namespace

// ===========================================================================
//  二、核心内部实现
// ===========================================================================
namespace {

// ---- 仓库地址：与旧版完全一致（提交入口 / 共享 ID 库）----
const wchar_t* const kSharedFsmDbUrl =
    L"https://raw.githubusercontent.com/2749478981/sonar/main/fsm_db.csv";
const wchar_t* const kSubmitIssueUrl =
    L"https://github.com/2749478981/WeaponSoundEnhance/issues/new"
    L"?title=%E5%8A%A8%E4%BD%9CID%E6%8F%90%E4%BA%A4&body=%E8%AF%B7%E6%8A%8A%20fsm_db_submission.csv%20%E7%9A%84%E5%86%85%E5%AE%B9%E7%B2%98%E5%9C%A8%E4%B8%8B%E9%9D%A2%EF%BC%88%E6%88%96%E4%BD%9C%E4%B8%BA%E9%99%84%E4%BB%B6%E4%B8%8A%E4%BC%A0%EF%BC%89%EF%BC%9A";
const char* const kReleasesPage = "https://github.com/2749478981/sonar/releases/latest";

std::string Trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string ToLowerAscii(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
    return s;
}

// 路径比较：忽略大小写与 / \\ 差异（判断"正在编辑的是不是游戏那份 ini"）
bool SamePath(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return true;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((std::size_t)(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string Utf8FromWide(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((std::size_t)(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// 逗号分隔拆分（ini 里 14 个武器开关用）
static void SplitCsv(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
}

// 长路径：界面里选出来的路径很容易超 MAX_PATH（mod 管理器/解包的深层目录）
std::wstring LongPathW(const std::string& utf8) {
    return FsLongPath(utf8);
}

// 当前场景（DLL 写 plugins\SonarScene.txt：mapId\nquestState；GUI 1s 缓存读取）
static int g_sceneMapId = -1;
static int g_sceneQuestState = -1;
    int   g_sceneQuestDmg = -1;   // 场景文件里的"任务累计伤害"（-1 = 读不到）
    float g_sceneMonHp = -1.0f;   // 怪物血量（-1 = 读不到）
    float g_sceneMonHpMax = -1.0f;

std::string ExeDir() {
    // 用动态缓冲：GetModuleFileNameW 在路径超 MAX_PATH 时返回 0，旧版固定
    // MAX_PATH 缓冲在长路径安装位置上会拿到空字符串，整个数据目录就废了。
    std::wstring ws;
    DWORD cap = 512;
    for (;;) {
        std::vector<wchar_t> buf(cap, L'\0');
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), cap);
        if (n == 0) break;
        if (n < cap - 1) { ws.assign(buf.data(), n); break; }
        if (cap >= 32768) { ws.assign(buf.data(), n); break; }
        cap *= 2;
    }
    const std::size_t p = ws.find_last_of(L"\\/");
    if (p != std::wstring::npos) ws = ws.substr(0, p + 1);
    return Utf8FromWide(ws);
}

bool IsAbsPath(const std::string& p) {
    if (p.size() >= 2 && p[1] == ':') return true;                             // C:\...
    if (p.size() >= 2 && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/'))
        return true;                                                           // UNC
    return false;
}

// 路径统一成正斜杠（写进 JSON 给前端用；前端只做显示与拼接）
std::string ToSlash(std::string p) {
    for (std::size_t i = 0; i < p.size(); ++i) if (p[i] == '\\') p[i] = '/';
    return p;
}
std::string ToBack(std::string p) {
    for (std::size_t i = 0; i < p.size(); ++i) if (p[i] == '/') p[i] = '\\';
    return p;
}

// 从自身目录往上找游戏根目录（含结尾反斜杠）：GUI 一般放在
// <游戏>\nativePC\plugins\WeaponSoundEnhance\，往上 1~6 层就能看到 exe。
bool FindGameDirFromSelf(std::string& out) {
    std::string d = ExeDir();
    for (int i = 0; i < 6 && !d.empty(); ++i) {
        if (FsExists(d + "MonsterHunterWorld.exe")) { out = d; return true; }
        std::string up = d;
        while (!up.empty() && (up[up.size() - 1] == '\\' || up[up.size() - 1] == '/')) up.erase(up.size() - 1);
        const std::size_t s = up.find_last_of("\\/");
        if (s == std::string::npos) break;
        d = up.substr(0, s + 1);
    }
    return false;
}

// 递归创建多级目录（已存在的忽略）
void MakeDirs(const std::wstring& dir) {
    if (dir.empty()) return;
    std::wstring cur;
    for (const wchar_t* p = dir.c_str(); *p; ++p) {
        cur += *p;
        if (*p == L'\\' || *(p + 1) == L'\0') ::CreateDirectoryW(cur.c_str(), nullptr);
    }
}

// 递归删除目录树
void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = ::FindFirstFileW((dir + L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring nm = fd.cFileName;
            if (nm == L"." || nm == L"..") continue;
            const std::wstring q = dir + nm;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(q + L"\\");
            else ::DeleteFileW(q.c_str());
        } while (::FindNextFileW(f, &fd));
        ::FindClose(f);
    }
    ::RemoveDirectoryW(dir.c_str());
}

// 宽路径读写（CreateFileW + 长路径前缀）
bool WriteWideFile(const std::wstring& path, const void* data, std::size_t n) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = (n == 0) || (::WriteFile(h, data, (DWORD)n, &wr, nullptr) && wr == (DWORD)n);
    ::CloseHandle(h);
    return ok;
}


// 原子复制：先写 .tmp，校验大小后再改名到位。
// ★ 直接 CopyFileW 的隐患：源文件正被编辑器写入时会留下**半截目标文件**，
//   用户看到的就是"音效文件损坏、还得重新添加"（实测反馈）。
bool CopyFileAtomic(const std::wstring& src, const std::wstring& dst) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fad)) return false;
    const unsigned long long srcSize = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (srcSize == 0) return false;
    const std::wstring tmp = dst + L".tmp";
    ::DeleteFileW(tmp.c_str());
    if (!::CopyFileW(src.c_str(), tmp.c_str(), FALSE)) { ::DeleteFileW(tmp.c_str()); return false; }
    WIN32_FILE_ATTRIBUTE_DATA t2{};
    if (!::GetFileAttributesExW(tmp.c_str(), GetFileExInfoStandard, &t2)) { ::DeleteFileW(tmp.c_str()); return false; }
    const unsigned long long tmpSize = ((unsigned long long)t2.nFileSizeHigh << 32) | t2.nFileSizeLow;
    if (tmpSize != srcSize) { ::DeleteFileW(tmp.c_str()); return false; }   // 大小不符：源正在被写，放弃
    if (!::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool ReadWideFile(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64LL << 20)) {
        ::CloseHandle(h);
        return false;
    }
    out.resize((std::size_t)sz.QuadPart);
    DWORD rd = 0;
    const bool ok = ::ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr) != FALSE && rd == out.size();
    ::CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

// 给 zip 里所有条目补上 UTF-8 文件名标志（GP 标志 bit11=0x0800）。
// miniz 1.x 不写这个标志，中文条目名在资源管理器里会按 ANSI 显示成乱码。
static void PatchZipUtf8Flags(std::uint8_t* p, std::size_t n) {
    std::size_t i = 0;
    while (i + 30 <= n) {
        if (p[i] == 0x50 && p[i + 1] == 0x4B && p[i + 2] == 0x03 && p[i + 3] == 0x04) {
            p[i + 7] |= 0x08;
            const std::size_t fn = p[i + 26] | ((std::size_t)p[i + 27] << 8);
            const std::size_t ex = p[i + 28] | ((std::size_t)p[i + 29] << 8);
            const std::size_t cs = (std::size_t)p[i + 18] | ((std::size_t)p[i + 19] << 8) |
                                   ((std::size_t)p[i + 20] << 16) | ((std::size_t)p[i + 21] << 24);
            i += 30 + fn + ex + cs;
        } else if (p[i] == 0x50 && p[i + 1] == 0x4B && p[i + 2] == 0x01 && p[i + 3] == 0x02) {
            p[i + 9] |= 0x08;
            const std::size_t fn = p[i + 28] | ((std::size_t)p[i + 29] << 8);
            const std::size_t ex = p[i + 30] | ((std::size_t)p[i + 31] << 8);
            const std::size_t cm = p[i + 32] | ((std::size_t)p[i + 33] << 8);
            i += 46 + fn + ex + cm;
        } else {
            break;
        }
    }
}

// 创建 zip：组合 txt（内存）+ soundsSrcDir 里所有文件（保留子目录）
bool ZipBuildArchive(const std::wstring& zipPath, const std::string& txtName,
                     const std::string& txtData, const std::wstring& soundsSrcDir,
                     std::string& err) {
    mz_zip_archive z{};
    if (!mz_zip_writer_init_heap(&z, 0, (std::size_t)1 << 20)) { err = "初始化 zip 失败"; return false; }
    if (mz_zip_writer_add_mem(&z, txtName.c_str(), txtData.data(), txtData.size(), 0) == MZ_FALSE) {
        err = "打包组合文件失败";
        mz_zip_writer_end(&z);
        return false;
    }
    bool fail = false;
    // 手写栈式遍历（不引 <functional> 的递归 lambda，纯为少一层依赖）
    std::vector<std::pair<std::wstring, std::string> > stack;
    stack.push_back(std::make_pair(soundsSrcDir, std::string("sounds/")));
    while (!stack.empty() && !fail) {
        const std::wstring disk = stack.back().first;
        const std::string arc = stack.back().second;
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE f = ::FindFirstFileW((disk + L"*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring nm = fd.cFileName;
            if (nm == L"." || nm == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(std::make_pair(disk + nm + L"\\", arc + Utf8FromWide(nm) + "/"));
                continue;
            }
            std::string bytes;
            if (!ReadWideFile(disk + nm, bytes)) continue;
            const std::string arcName = arc + Utf8FromWide(nm);
            if (mz_zip_writer_add_mem(&z, arcName.c_str(), bytes.data(), bytes.size(), 0) == MZ_FALSE) {
                err = "打包音效失败: " + arcName;
                fail = true;
                break;
            }
        } while (::FindNextFileW(f, &fd));
        ::FindClose(f);
    }
    if (fail) { mz_zip_writer_end(&z); return false; }

    void* buf = nullptr;
    std::size_t bufsz = 0;
    if (mz_zip_writer_finalize_heap_archive(&z, &buf, &bufsz) == MZ_FALSE) {
        err = "取 zip 内容失败";
        mz_zip_writer_end(&z);
        return false;
    }
    mz_zip_writer_end(&z);
    PatchZipUtf8Flags((std::uint8_t*)buf, bufsz);
    const bool okW = WriteWideFile(zipPath, buf, bufsz);
    mz_free(buf);
    if (!okW) { err = "写 zip 文件失败: " + Utf8FromWide(zipPath); return false; }
    return true;
}

// 解开 zip 到 extDir；返回其中第一个 *.txt/*.ini 的完整路径
std::string ZipExtractTo(const std::wstring& zipPath, const std::string& extDir) {
    std::string bytes;
    if (!ReadWideFile(zipPath, bytes)) return std::string();
    mz_zip_archive z{};
    if (mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0) == MZ_FALSE) return std::string();
    std::string found;
    const mz_uint num = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < num; ++i) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&z, i, &st) == MZ_FALSE) continue;
        const std::string name = st.m_filename;
        if (name.empty() || name.find("..") != std::string::npos) continue;   // 防目录穿越
        std::size_t len = 0;
        void* buf = mz_zip_reader_extract_to_heap(&z, i, &len, 0);
        if (!buf) continue;
        std::string rel = ToBack(name);
        if (!rel.empty() && rel[0] == '\\') rel.erase(0, 1);
        const std::string outPath = extDir + rel;
        const std::size_t s = outPath.find_last_of("\\/");
        if (s != std::string::npos) MakeDirs(Utf8ToWide(outPath.substr(0, s + 1)));
        WriteWideFile(Utf8ToWide(outPath), buf, len);
        mz_free(buf);
        if (found.empty()) {
            const std::string low = ToLowerAscii(name);
            const std::size_t dot = low.find_last_of('.');
            const std::string ext = (dot == std::string::npos) ? "" : low.substr(dot);
            if (ext == ".txt" || ext == ".ini") found = outPath;
        }
    }
    mz_zip_reader_end(&z);
    return found;
}

// ---------------------------------------------------------------------------
//  试听：后台线程解码 + waveOut 播放
//
//  为什么另开线程：解码一个几十兆的 flac 要几百毫秒，在主线程做会把界面卡住；
//  waveOut 又是阻塞式的（要等 WHDR_DONE），更不能占着 UI 线程。
//  这里和旧版一样用裸 CreateThread（不引 std::thread 的 join 语义，播完自己退出）。
// ---------------------------------------------------------------------------
struct PvCtx {
    std::wstring path;
    float gain = 1.0f;
    unsigned delayMs = 0;
};

DWORD WINAPI PvWorker(LPVOID param) {
    std::unique_ptr<PvCtx> ctx((PvCtx*)param);
    if (ctx->delayMs > 0) ::Sleep(ctx->delayMs);

    HANDLE h = ::CreateFileW(ctx->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64LL << 20)) {
        ::CloseHandle(h);
        return 0;
    }
    std::vector<std::uint8_t> raw((std::size_t)sz.QuadPart);
    DWORD rd = 0;
    if (!::ReadFile(h, raw.data(), (DWORD)raw.size(), &rd, nullptr) || rd != (DWORD)raw.size()) {
        ::CloseHandle(h);
        return 0;
    }
    ::CloseHandle(h);

    // 统一解码：wav(含 24/32bit、float) / mp3 / ogg / flac 都能播
    wseaudio::Pcm pcm;
    if (!wseaudio::DecodeFileBytes(raw.data(), raw.size(), pcm) || pcm.samples.empty()) return 0;

    float g = ctx->gain < 0.0f ? 0.0f : (ctx->gain > 1.0f ? 1.0f : ctx->gain);
    if (g < 1.0f)
        for (std::size_t i = 0; i < pcm.samples.size(); ++i)
            pcm.samples[i] = (std::int16_t)(pcm.samples[i] * g);

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)pcm.channels;
    wfx.nSamplesPerSec = (DWORD)pcm.rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)((16 / 8) * pcm.channels);
    wfx.nAvgBytesPerSec = pcm.rate * wfx.nBlockAlign;
    HWAVEOUT hwo = nullptr;
    if (::waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return 0;

    std::vector<std::uint8_t> bytes(pcm.samples.size() * 2);
    std::memcpy(bytes.data(), pcm.samples.data(), bytes.size());
    WAVEHDR hdr{};
    hdr.lpData = (LPSTR)bytes.data();
    hdr.dwBufferLength = (DWORD)bytes.size();
    if (::waveOutPrepareHeader(hwo, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) {
        ::waveOutClose(hwo);
        return 0;
    }
    ::waveOutWrite(hwo, &hdr, sizeof(hdr));
    while ((hdr.dwFlags & WHDR_DONE) == 0) ::Sleep(10);
    ::waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr));
    ::waveOutClose(hwo);
    return 0;
}

void PlayPreviewFile(const std::wstring& path, float gain, unsigned delayMs) {
    PvCtx* c = new PvCtx();
    c->path = path;
    c->gain = gain;
    c->delayMs = delayMs;
    HANDLE th = ::CreateThread(nullptr, 0, &PvWorker, c, 0, nullptr);
    if (th) ::CloseHandle(th);
    else delete c;
}

// ---------------------------------------------------------------------------
//  界面偏好（Sonar_UI.json）
//
//  旧版这些状态是交给 ImGui 自己的 settings ini 记的（列宽、排序、窗口尺寸），
//  WebView2 没有那一套，所以核心自己存一份 JSON：主题、列宽、排序、窗口几何、
//  武器筛选、捕获历史是否展开。写盘是"变了就写"，文件很小（不到 1KB）。
// ---------------------------------------------------------------------------
struct UiPrefs {
    std::string theme = "clean-light";
    JVal colWidths = JVal::obj();
    JVal sort = JVal::obj();
    JVal window = JVal::obj();
    int weaponFilter = -1;
    bool histExpanded = false;
    bool onlyActive = false;   // 列表默认只看当前激活组合的条目
    bool wemView = false;      // 捕获面板视图：false=派生 fsm，true=WEM 音效
    std::string playerPath;    // 试听用的播放器（如 foobar2000 + vgmstream）

    UiPrefs() {
        colWidths.set("name", JVal(180));
        colWidths.set("weapon", JVal(80));
        colWidths.set("lmt", JVal(110));
        colWidths.set("fsm", JVal(90));
        colWidths.set("sound", JVal(220));

        sort.set("key", JVal(std::string("name")));
        sort.set("dir", JVal(1));

        window.set("x", JVal(-1));
        window.set("y", JVal(-1));
        window.set("w", JVal(1280));
        window.set("h", JVal(820));
        window.set("max", JVal(false));
    }
};

// 主题清单：id 就是前端 data-theme 的值，name/desc 给用户看（中文）
struct ThemeInfo { const char* id; const char* name; const char* desc; };
const ThemeInfo kThemes[] = {
    { "pro-dark",        "深色专业", "深色专业" },
    { "clean-light",     "极简浅色", "极简浅色" },
    { "cute-pastel",     "可爱风",   "可爱风" },
    { "sticker-outline", "描边贴纸", "描边贴纸" },
};
const int kThemeCount = 4;
const char* const kDefaultTheme = "clean-light";

bool IsValidThemeId(const std::string& id) {
    for (int i = 0; i < kThemeCount; ++i)
        if (id == kThemes[i].id) return true;
    return false;
}

JVal ThemesJson() {
    JVal arr = JVal::arr();
    for (int i = 0; i < kThemeCount; ++i) {
        JVal t = JVal::obj();
        t.set("id", JVal(std::string(kThemes[i].id)));
        t.set("name", JVal(std::string(kThemes[i].name)));
        t.set("desc", JVal(std::string(kThemes[i].desc)));
        arr.push(t);
    }
    return arr;
}

// ---- 条件表达式：变量表 / 比较符（必须和插件侧 WeaponSoundEnhance.cpp 的
//      cond::kVarNames 完全一致，否则写进 ini 的条件插件认不出来）----
const char* const kCondVars[] = {
    "dmg", "aura", "dAura", "charge", "dCharge", "lmt", "fsm", "fsmTarget", "ms"
};
const char* const kCondOps[] = { ">", ">=", "<", "<=", "==", "!=" };

// 把条件表达式规范化成 "a & b | c" 形式：去掉多余空格、统一分隔符写法。
// 不认识的部分原样保留 —— 手写的复杂条件插件能认，界面不该把它吃掉。
std::string NormalizeExpr(const std::string& in) {
    // 先按 & | 切段，每段再规整成 "变量 运算符 数值"
    std::string out;
    std::string cur;
    bool pendingOr = false;
    bool first = true;
    for (std::size_t i = 0; i <= in.size(); ++i) {
        const char c = (i < in.size()) ? in[i] : (char)0;
        if (c == '&' || c == '|' || c == 0) {
            const std::string t = Trim(cur);
            cur.clear();
            if (!t.empty()) {
                if (!first) out += pendingOr ? " | " : " & ";
                first = false;
                out += t;
            }
            pendingOr = (c == '|');
            if (c == 0) break;
        } else {
            cur += c;
        }
    }
    return out;
}

// 条件是否"看起来合法"：变量名在表里、运算符在表里、值是整数。
// 不合法也不算错（前端允许用户自己写），只是不要往 ini 里塞空条件。
bool CondLooksValid(const std::string& expr) {
    std::string cur;
    bool any = false;
    for (std::size_t i = 0; i <= expr.size(); ++i) {
        const char c = (i < expr.size()) ? expr[i] : (char)0;
        if (c == '&' || c == '|' || c == 0) {
            const std::string t = Trim(cur);
            cur.clear();
            if (!t.empty()) {
                const std::size_t op = t.find_first_of("<>=!");
                if (op == std::string::npos || op == 0) return false;
                bool varOk = false;
                const std::string vn = Trim(t.substr(0, op));
                for (int k = 0; k < 9; ++k) if (vn == kCondVars[k]) { varOk = true; break; }
                if (!varOk) return false;
                any = true;
            }
            if (c == 0) break;
        } else {
            cur += c;
        }
    }
    return any;
}

// ---- SoundSpec / SoundEntry <-> JSON ----
JVal SpecToJson(const SoundSpec& s) {
    JVal v = JVal::obj();
    v.set("path", JVal(ToSlash(s.path)));
    v.set("delay", JVal(s.delay));
    v.set("vol", JVal(s.vol));
    v.set("fixed", JVal(s.fixed));
    v.set("cdMs", JVal(s.cdMs));
    v.set("playLock", JVal(s.playLock));
    return v;
}

SoundSpec SpecFromJson(const JVal& v) {
    SoundSpec s;
    if (v.isStr()) {                       // 也容忍前端直接传一个路径字符串
        s.path = v.s;
        return s;
    }
    s.path = v.optStr("path");
    s.delay = v.optInt("delay", 0);
    if (s.delay < 0) s.delay = 0;
    s.vol = v.optInt("vol", 100);
    if (s.vol < 0) s.vol = 0;
    if (s.vol > 100) s.vol = 100;
    s.fixed = v.optBool("fixed", false);
    s.cdMs = v.optInt("cdMs", 0);
    if (s.cdMs < 0) s.cdMs = 0;
    if (s.cdMs > 60000) s.cdMs = 60000;
    s.playLock = v.optBool("playLock", false);
    return s;
}

JVal PoolToJson(const PoolSpec& p) {
    JVal a = JVal::arr();
    for (std::size_t i = 0; i < p.specs.size(); ++i) a.push(SpecToJson(p.specs[i]));
    return a;
}

void PoolFromJson(const JVal& v, PoolSpec& out) {
    out.specs.clear();
    if (!v.isArr()) return;
    for (std::size_t i = 0; i < v.a.size(); ++i) {
        SoundSpec s = SpecFromJson(v.a[i]);
        if (s.path.empty()) continue;                       // 空路径等于没填
        bool dup = false;
        for (std::size_t k = 0; k < out.specs.size(); ++k)
            if (out.specs[k].path == s.path) { dup = true; break; }
        if (dup) continue;                                  // 同池重复路径没有意义
        out.specs.push_back(s);
    }
}

JVal EntryToJson(const SoundEntry& e, int index) {
    JVal v = JVal::obj();
    v.set("index", JVal(index));
    v.set("name", JVal(e.name));
    v.set("weaponType", JVal(e.weaponType));
    v.set("weaponName", JVal(std::string(WeaponName(e.weaponType))));
    v.set("combo", JVal(e.combo));
    v.set("fsmId", JVal(e.fsmId));
    v.set("fsmTarget", JVal(e.fsmTarget));
    v.set("media", JVal(e.media));
    v.set("bank", JVal(e.bank));
    v.set("group", JVal(e.group));
    v.set("stop", JVal(e.stop));

    JVal lmt = JVal::arr();
    for (std::size_t i = 0; i < e.lmt.size(); ++i) lmt.push(JVal(e.lmt[i]));
    v.set("lmt", lmt);
    v.set("lmtAny", e.lmt.empty());

    v.set("def", PoolToJson(e.def));
    JVal g = JVal::arr();
    for (int i = 0; i < 4; ++i) g.push(PoolToJson(e.gauge[i]));
    v.set("gauge", g);

    v.set("checkDelayMs", JVal(e.checkDelayMs));
    v.set("checkTimeoutMs", JVal(e.checkTimeoutMs));
    v.set("checkOffsetMs", JVal(e.checkOffsetMs));
    v.set("endOnAction", JVal(e.endOnAction));
    v.set("checkMode", JVal(e.checkMode));
    v.set("judgePreset", JVal(e.judgePreset));

    JVal conds = JVal::arr();
    for (std::size_t i = 0; i < e.conds.size(); ++i) {
        JVal c = JVal::obj();
        c.set("expr", JVal(e.conds[i].expr));
        c.set("atEnd", JVal(e.conds[i].atEnd));
        c.set("pool", PoolToJson(e.conds[i].pool));
        conds.push(c);
    }
    v.set("conds", conds);
    // 音效总数的便捷字段：前端列表页要显示"3 个音效"，省得它在 JS 里数三层
    int n = (int)e.def.specs.size();
    for (int i = 0; i < 4; ++i) n += (int)e.gauge[i].specs.size();
    for (std::size_t i = 0; i < e.conds.size(); ++i) n += (int)e.conds[i].pool.specs.size();
    v.set("soundCount", JVal(n));
    return v;
}

// 从 JSON 反序列化条目。base 是"新增时的默认值"。
// allowedCombos 为空表示不做组合合法性检查（用于导入等内部路径）。
SoundEntry EntryFromJson(const JVal& v, const std::vector<std::string>& allowedCombos) {
    SoundEntry e;
    e.weaponType = v.optInt("weaponType", -1);
    if (e.weaponType < -1 || e.weaponType > 13) e.weaponType = -1;
    e.name = Trim(v.optStr("name"));
    e.group = Trim(v.optStr("group"));
    e.stop = v.optBool("stop", false);
    e.fsmId = v.optInt("fsmId", -1);
    e.fsmTarget = v.optInt("fsmTarget", -1);
    e.media = (long long)v.optInt("media", -1);   // wem/media id；id < 2^31
    e.bank = Trim(v.optStr("bank"));

    // 组合归属：任意武器没有组合概念，固定进默认组合
    //（否则保存时会被丢进一个不存在的 [Weapon-1:名字] 段）
    if (e.weaponType < 0) {
        e.combo.clear();
    } else {
        const std::string want = Trim(v.optStr("combo"));
        if (want.empty()) {
            e.combo.clear();
        } else if (allowedCombos.empty()) {
            e.combo = want;
        } else {
            bool legal = false;
            for (std::size_t i = 0; i < allowedCombos.size(); ++i)
                if (allowedCombos[i] == want) { legal = true; break; }
            // 组合名不属于这把武器 → 回默认组合。旧版这里放行过，结果造出
            // "武器6 的组合 DIO" 这种跨武器串台的条目，读回来谁都找不到。
            e.combo = legal ? want : std::string();
        }
    }

    // LMT：勾了"不限"或列表为空 → 空列表（= 任意 LMT）。
    // 也接受前端传字符串（"49265,49256"），省得 JS 侧还要自己拆数字。
    const JVal* jlmt = v.find("lmt");
    if (jlmt && jlmt->isStr()) {
        std::string cur;
        const std::string s = jlmt->s;
        for (std::size_t i = 0; i <= s.size(); ++i) {
            const char c = (i < s.size()) ? s[i] : (char)0;
            if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == 0) {
                const std::string t = Trim(cur);
                cur.clear();
                if (!t.empty()) {
                    std::string low = ToLowerAscii(t);
                    const bool wild = (low == "-1" || low == "any" || low == "all" || low == "*" ||
                                       t == "不限");
                    if (wild) { e.lmt.clear(); break; }
                    const long val = std::atol(t.c_str());
                    if (val >= 0) {
                        bool dup = false;
                        for (std::size_t k = 0; k < e.lmt.size(); ++k)
                            if (e.lmt[k] == (int)val) { dup = true; break; }
                        if (!dup) e.lmt.push_back((int)val);
                    }
                }
                if (c == 0) break;
            } else {
                cur += c;
            }
        }
    } else if (jlmt && jlmt->isArr()) {
        for (std::size_t i = 0; i < jlmt->a.size(); ++i) {
            const int val = jlmt->a[i].asInt(-1);
            if (val < 0) continue;
            bool dup = false;
            for (std::size_t k = 0; k < e.lmt.size(); ++k)
                if (e.lmt[k] == val) { dup = true; break; }
            if (!dup) e.lmt.push_back(val);
        }
    }
    // ★ 以 lmt 数组为准：lmtAny 只是前端的"不限"复选状态，不能反过来清数据
        //   （曾经 lmtAny=true 就清空 lmt → wem 条目填的 LMT 一保存就丢）
        if (!jlmt) e.lmt.clear();

    const JVal* jdef = v.find("def");
    if (jdef) PoolFromJson(*jdef, e.def);
    const JVal* jg = v.find("gauge");
    if (jg && jg->isArr()) {
        for (int i = 0; i < 4 && i < (int)jg->a.size(); ++i) PoolFromJson(jg->a[i], e.gauge[i]);
    }

    // 判定：只有存在判定条件时这些字段才有意义，但仍原样保留（用户可能先填参数后加条件）
    e.checkDelayMs = v.optInt("checkDelayMs", 0);
    e.checkTimeoutMs = v.optInt("checkTimeoutMs", 0);
    e.checkOffsetMs = v.optInt("checkOffsetMs", 150);
    e.endOnAction = v.optBool("endOnAction", true);
    e.checkMode = v.optInt("checkMode", 0);
    e.judgePreset = v.optInt("judgePreset", 0);

    const JVal* jc = v.find("conds");
    if (jc && jc->isArr()) {
        for (std::size_t i = 0; i < jc->a.size(); ++i) {
            const JVal& cv = jc->a[i];
            CondSpec c;
            c.expr = NormalizeExpr(cv.optStr("expr"));
            c.atEnd = cv.optBool("atEnd", false);
            const JVal* jp = cv.find("pool");
            if (jp) PoolFromJson(*jp, c.pool);
            // 空表达式或空音效池的条件写进 ini 只会添乱（插件会当成"永远不成立"），丢掉
            if (c.expr.empty() || c.pool.specs.empty()) continue;
            if (!CondLooksValid(c.expr)) {
                // 表达式看不懂：仍然写进去，插件侧若认不出会忽略这条。
                // 但只要有一条能用的，就别把整条配置判废 —— 用户可能手写了高级条件。
            }
            e.conds.push_back(c);
        }
    }
    if (e.conds.empty()) {
        // 没有条件就没有判定窗口，把窗口参数清零，免得 ini 里留下无效的 CheckTimeout
        e.checkTimeoutMs = 0;
    } else if (e.checkTimeoutMs <= 0) {
        e.checkTimeoutMs = 2500;   // 和旧版一样给一个默认窗口
    }
    if (e.checkOffsetMs < 0) e.checkOffsetMs = 0;
    if (e.checkDelayMs < 0) e.checkDelayMs = 0;
    return e;
}

// 条目去重键：武器 + 组合 + FSM + 目标层 + LMT + 全部音效路径。
// （旧版就是这个规则，导入组合包/合并旧 ini 时用它判重。）
std::string EntryKey(const SoundEntry& e) {
    std::string k = std::to_string(e.weaponType) + "|" + e.combo + "|" +
                    std::to_string(e.fsmId) + "|" + std::to_string(e.fsmTarget) + "|";
    for (std::size_t i = 0; i < e.lmt.size(); ++i) k += std::to_string(e.lmt[i]) + ",";
    k += "|";
    for (std::size_t i = 0; i < e.def.specs.size(); ++i) k += e.def.specs[i].path + ";";
    for (int i = 0; i < 4; ++i)
        for (std::size_t j = 0; j < e.gauge[i].specs.size(); ++j) k += e.gauge[i].specs[j].path + ";";
    for (std::size_t i = 0; i < e.conds.size(); ++i)
        for (std::size_t j = 0; j < e.conds[i].pool.specs.size(); ++j) k += e.conds[i].pool.specs[j].path + ";";
    return k;
}

// 组合名 → 合法文件夹名（去掉 Windows 不允许的字符）
std::string ComboFolder(const std::string& combo) {
    std::string nm = combo.empty() ? std::string("默认组合") : combo;
    for (std::size_t i = 0; i < nm.size(); ++i) {
        const char c = nm[i];
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') nm[i] = '_';
    }
    return nm;
}

std::string TimeNowHms() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char tb[16];
    snprintf(tb, sizeof(tb), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return std::string(tb);
}

std::string TempSubDir(const char* tag) {
    char t[MAX_PATH] = {};
    ::GetTempPathA(MAX_PATH, t);
    return std::string(t) + tag + "_" +
           std::to_string((unsigned long)::GetCurrentProcessId()) + "\\";
}

} // namespace

// ---------------------------------------------------------------------------
//  三、Core::Impl
//
//  ★ 位置说明（不是随手排的）：Impl 是 Core 的**私有**嵌套类型，本文件里那些
//    静态辅助函数（文件对话框、试听、组合打包、ini 合并）都需要一个 Impl*，
//    但按 C++ 的访问规则，它们不能在自己的签名里写 Core::Impl*（那是私有名字）。
//    所以把这些辅助函数做成 Impl 的**静态成员函数**，定义在 Impl 类体内部；
//    类体因此必须排在辅助函数之后、Handle() 之前。这样既不改头文件，
//    也不用 reinterpret_cast 去硬掰类型系统。
// ---------------------------------------------------------------------------
struct Core::Impl {
    GameReader game;
    LiveState live;
    bool liveOk = false;
    Config cfg;                  // 配置本体（LoadConfig/SaveConfig 直接操作它）
    std::string status;          // 最近一次操作的结果（前端也可从各方法返回值拿）

    std::string exeDir;          // 本模块目录（含结尾反斜杠）
    std::string gameIniPath;     // 游戏实际读取的那份 ini（能定位到才有值）
    std::string uiPath;          // <exeDir>Sonar_UI.json

    UiPrefs prefs;

    // 实时捕获历史（最多 64 条）
    struct HistEntry {
        int fsm = -1;
        int lmt = -1;
        int weapon = -1;
        int weaponId = -1;
        int kind = 0;           // 0 = 派生（fsm/lmt），1 = wem
        int wemMedia = -1;      // kind=1：WWise media id
        std::string wemName;    // kind=1：可读名（bank/path）
        std::string bank;       // kind=1：nbnk bank 名（wp_bow_cmn）
        // kind=2：特效捕获（表 · 组 · 组内记录下标）
        std::string efxTable;
        int efxGroup = -1;
        int efxRec = -1;
        int efxRecId = -1;
        int efxVia = -1;
        std::string efxCand;   // 同一 (组,记录) 可能属于多张表：候选表名
        std::string efxPath;
        std::string time;
        unsigned long long ms = 0;     // GetTickCount64 时间戳，用于"短时间同动作合并"
    };
    // ★ 两条独立列表：派生捕获 / wem 捕获。以前共用一个 vector，
    //   wem 事件一多就把派生记录挤出去（用户看到的"派生不见了"）。
    std::vector<HistEntry> derHist;   // kind=0 派生（fsm/lmt）
    std::vector<HistEntry> wemHist;   // kind=1 wem
    int derRev = 0, wemRev = 0;       // 修订号：变了才让前端重绘（避免"没变化就不推送"卡死）
    int derDup = 0, wemDup = 0;       // 被去重丢弃的条数（界面统计）
    int wemLoopDrop = 0;              // 被判循环音静默丢弃的条数（界面统计）

    // ---- wem 捕获（读 SonarAudio.log，GUI 进程内即可，不进游戏）----
    std::string wemLogPath;                                        // plugins\SonarAudio.log
    std::string wemMapPath;                                        // plugins\SonarAudio.mediaids.txt
    std::string wemSeqPath;                                        // exeDir\WseMediaSeqs.txt（media_id→bank,序号 真值表）
    // 用户自定义 wem 名字库（不随包覆盖）：plugins\WeaponSoundEnhance\WseWemNames_user.txt
    //   格式：media_id <TAB> 名字；优先级高于随包的官方映射
    std::string wemUserNamePath;
    std::unordered_map<int, std::string> wemUserNames;
    std::string assetsDir;      // 内嵌资源解压目录（%LOCALAPPDATA%\Sonar\assets）
    std::unordered_map<int, std::string> wemNames;                 // media id -> 可读名(UTF-8)
    std::unordered_map<int, std::string> wemNotes;                 // media id -> 括号里的注释名（瓶子声/弹刀…）
    std::unordered_map<int, std::pair<std::string, int>> wemSeqs;  // media id -> (bank, 序号=该bank DIDX第几个)
    // 循环音检测（行为判定）：4 秒内同一 media 出现 >= 4 次 → 判为循环音并静默 8 秒
    struct LoopStat { unsigned long long winStart = 0; int count = 0; unsigned long long muteUntil = 0; };
    std::unordered_map<int, LoopStat> wemLoops;
    int wemOkCount = 0;      // 识别成功（media>0）的事件数（界面统计）
    int wemZeroCount = 0;    // 反查不到 bank（media=0）被跳过的事件数（界面统计）
    std::vector<unsigned char> playBuf;   // GUI 内试听：解码后的 wav（播放期间必须保活）
    std::ifstream wemStream;
    unsigned long long wemLastPoll = 0;
    long long lastWemMedia = -2;                                   // 推送去重
    int lastEfxRev = -1, lastWemRev = -1, lastDerRev = -1;   // 捕获列表修订号（变化也要推送！）
    int curWemMedia = -1;                                          // 最近一条 wem
    std::string curWemName;
    std::string curWemBank;
    int curWemWeapon = -1;
    bool hasWemNames = false;

    // ---- 特效捕获（实验性）：读 plugins\WeaponSoundEnhance_efx.log ----
    std::vector<HistEntry> efxHist;   // kind=2 特效
    int efxRev = 0, efxDup = 0;
    int efxOkCount = 0;
    std::ifstream efxStream;
    unsigned long long efxLastPoll = 0;
    std::string efxLogPath;
    std::string efxStatus;            // DLL 自检行（hook=ok … / 失败原因）
    unsigned long long efxLogPos = 0;   // 已读到哪（用于检测日志被清空/重建）
    std::string efxIndexInfo;         // 索引来源与规模（# sonarefx index: 行）
    std::string efxStats;             // DLL 定期统计行（calls/weapon/other/dup）
    std::string efxRawSample;         // 不在索引里的原始 (组,记录) 取样（诊断用）
    int efxRawCount = 0;
    std::string curEfxTable;          // 最近一条特效
    int curEfxGroup = -1;
    int curEfxRec = -1;
    int curEfxVia = -1;
    int curEfxWeapon = -1;
    std::string curEfxPath;

    unsigned long long lastPoll = 0;    // 按 ini 的 PollMs 节流
    bool dirty = false;                 // cfg 有未保存改动
    bool uiDirty = false;               // UI 偏好有改动待落盘

    // ---- 上一次推送给前端的状态（只在变化时推，避免刷屏）----
    bool hadLive = false;
    bool lastAttached = false;
    bool lastInScene = false;
    std::string lastError;
    int lastFsm = -2, lastLmt = -2, lastWeapon = -2, lastWeaponId = -2;
    unsigned long long lastHistPush = 0;

    // ---- 更新：后台线程 + 原子量（唯一需要线程的地方）----
    std::atomic<int> updState{0};        // 0=未查 1=进行中 2=已最新 3=有新版 4=出错
    std::atomic<int> updProgress{-1};    // -1 = 未知总长
    std::atomic<bool> updInstalling{false};
    std::atomic<bool> updNeedRestart{false};
    std::atomic<bool> updSameVer{false};   // 同版本号但仓库资产更新（热修复包）
    std::atomic<bool> updIniPending{false};
    std::atomic<bool> updStarted{false};
    std::mutex updMx;
    std::string updTag, updUrl, updNotes, updErr, updMsg;

    // 构造函数里完成：定位 exe 目录 → 定位游戏 ini → 加载配置 → 载入 UI 偏好
    Impl() {
        exeDir = ExeDir();
        uiPath = exeDir + "Sonar_UI.json";

        std::string gdir;
        bool haveGame = FindGameExeDir(gdir);
        if (!haveGame) haveGame = FindGameDirFromSelf(gdir);
        if (haveGame)
            gameIniPath = gdir + "nativePC\\plugins\\WeaponSoundEnhance\\WeaponSoundEnhance.ini";

        // 候选顺序和旧版一致：优先"游戏实际读的那份"，因为 GUI 常被放在发布包
        // 解压目录里，先找 exe 同目录会导致"GUI 在改 A、游戏在读 B"，怎么改都没反应。
        std::vector<std::string> cand;
        if (haveGame) {
            cand.push_back(gameIniPath);
            cand.push_back(gdir + "nativePC\\plugins\\WeaponSoundEnhance.ini");
        }
        cand.push_back(exeDir + "WeaponSoundEnhance\\WeaponSoundEnhance.ini");
        cand.push_back(exeDir + "WeaponSoundEnhance.ini");

        for (std::size_t i = 0; i < cand.size(); ++i) {
            if (LoadConfig(cand[i], cfg)) {
                EnrichNames();
                SetFsmDbDir(BaseDir());
                ReloadFsmDb();
                status = "已加载: " + cand[i];
                break;
            }
        }
        if (!cfg.loaded) {
            // 没有用户 ini 时用随包模板播种一份（更新不会覆盖用户 ini）
            const std::string tpl = exeDir + "WeaponSoundEnhance.ini.template";
            const std::string ini = exeDir + "WeaponSoundEnhance.ini";
            if (!FsExists(ini) && FsExists(tpl) && FsCopy(tpl, ini) && LoadConfig(ini, cfg)) {
                EnrichNames();
                SetFsmDbDir(BaseDir());
                ReloadFsmDb();
                status = "已由模板生成并加载: " + ini;
            }
        }
        if (!cfg.loaded) {
            cfg.path = exeDir + "WeaponSoundEnhance\\WeaponSoundEnhance.ini";
            status = "未找到 ini，请点【打开 ini】选择 WeaponSoundEnhance.ini"
                     "（建议放在 plugins\\WeaponSoundEnhance\\ 下）";
        }

        LoadUiPrefs();
        InitWemWatch();     // 定位 SonarAudio.log / mediaids.txt / 序号真值表
        LoadWemMap();
        LoadWemSeqs();
        // 用户自定义名字库（优先级高于官方映射）
        wemUserNamePath = BaseDir() + "WseWemNames_user.txt";
        LoadWemUserNames();
        ExtractEmbeddedAssets();   // 内嵌资源（图标 / logo / 成就音效）解压到 %LOCALAPPDATA%
    }

    ~Impl() { game.Detach(); }

    // ---- 路径 ----
    // 数据目录 = ini 所在目录（插件读 sounds\ 也相对这里）
    std::string BaseDir() const {
        if (cfg.loaded && !cfg.path.empty()) {
            const std::size_t s = cfg.path.find_last_of("\\/");
            if (s != std::string::npos) return cfg.path.substr(0, s + 1);
        }
        return exeDir;
    }
    std::string IniPath() const {
        if (!cfg.path.empty()) return cfg.path;
        return exeDir + "WeaponSoundEnhance\\WeaponSoundEnhance.ini";
    }
    bool IsGameIni() const { return !gameIniPath.empty() && SamePath(IniPath(), gameIniPath); }

    // ---- 条目 / 组合 ----
    std::string ActiveCombo(int w) const {
        std::map<int, std::string>::const_iterator it = cfg.active.find(w);
        return (it == cfg.active.end()) ? std::string() : it->second;
    }
    bool EntryActive(const SoundEntry& e) const {
        if (e.weaponType < 0) return e.combo.empty();          // 任意武器：只有默认组合
        return e.combo == ActiveCombo(e.weaponType);
    }
    int CountFor(int w) const {
        int c = 0;
        for (std::size_t i = 0; i < cfg.entries.size(); ++i)
            if (cfg.entries[i].weaponType == w && EntryActive(cfg.entries[i])) ++c;
        return c;
    }
    // 该武器的组合列表：""（默认）永远在最前，命名组合按在 entries 里首次出现的顺序
    std::vector<std::string> WeaponCombos(int w) const {
        std::vector<std::string> v;
        v.push_back("");
        // 先把持久化列表加进去（空组合也在这，否则新增组合会被当成不存在）
        const std::map<int, std::vector<std::string> >::const_iterator it = cfg.comboList.find(w);
        if (it != cfg.comboList.end()) {
            for (std::size_t k = 0; k < it->second.size(); ++k) {
                bool has = false;
                for (std::size_t j = 0; j < v.size(); ++j) if (v[j] == it->second[k]) { has = true; break; }
                if (!has) v.push_back(it->second[k]);
            }
        }
        // 再合并条目里出现、但列表里没有的组合（老配置兼容，首次保存时补录）
        for (std::size_t i = 0; i < cfg.entries.size(); ++i) {
            const SoundEntry& e = cfg.entries[i];
            if (e.weaponType != w || e.combo.empty()) continue;
            bool has = false;
            for (std::size_t k = 0; k < v.size(); ++k) if (v[k] == e.combo) { has = true; break; }
            if (!has) v.push_back(e.combo);
        }
        return v;
    }
    // 编辑器/前端能不能把条目放进这个组合
    std::vector<std::string> AllowedCombos(int weapon) const {
        return (weapon < 0) ? std::vector<std::string>() : WeaponCombos(weapon);
    }

    void EnrichNames() {
        for (std::size_t i = 0; i < cfg.entries.size(); ++i) {
            SoundEntry& e = cfg.entries[i];
            if (e.name.empty()) e.name = LookupFsmName(e.weaponType, e.fsmId, e.LmtAny());
        }
    }

    // 名字解析：用户自己配的名字优先（改完立刻在捕获面板显示），知识库兜底。
    // 先精确匹配 LMT，再匹配 LMT 不限的通配条目。
    std::string ResolveName(int weapon, int fsm, int lmt) const {
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 0; i < cfg.entries.size(); ++i) {
                const SoundEntry& e = cfg.entries[i];
                if (!EntryActive(e)) continue;
                if (e.fsmId != fsm) continue;
                if (e.weaponType >= 0 && e.weaponType != weapon) continue;
                if (pass == 0) {
                    bool exact = false;
                    for (std::size_t k = 0; k < e.lmt.size(); ++k) if (e.lmt[k] == lmt) { exact = true; break; }
                    if (!exact) continue;
                } else {
                    if (!e.lmt.empty()) continue;
                }
                if (!e.name.empty()) return e.name;
            }
        }
        return LookupFsmName(weapon, fsm, lmt);
    }

    // ---- UI 偏好读写 ----
    void LoadUiPrefs() {
        std::string txt;
        if (!FsRead(uiPath, txt)) return;              // 没有就是全默认
        JsonParser p(txt);
        JVal v;
        if (!p.parse(v) || !v.isObj()) return;
        const std::string th = v.optStr("theme", kDefaultTheme);
        if (IsValidThemeId(th)) prefs.theme = th;      // 认不出的主题一律回默认，别把界面搞白屏
        const JVal* cw = v.find("colWidths");
        if (cw && cw->isObj()) prefs.colWidths = *cw;
        const JVal* so = v.find("sort");
        if (so && so->isObj()) prefs.sort = *so;
        const JVal* wi = v.find("window");
        if (wi && wi->isObj()) prefs.window = *wi;
        prefs.weaponFilter = v.optInt("weaponFilter", -1);
        if (prefs.weaponFilter < -2 || prefs.weaponFilter > 13) prefs.weaponFilter = -1;
        prefs.histExpanded = v.optBool("histExpanded", false);
        prefs.onlyActive = v.optBool("onlyActive", false);
        prefs.wemView = false;   // ★ 不持久化：每次启动都回到「派生捕获」视图
                                 //   （以前记住 WEM 视图，导致用户以为派生捕获"不见了"）
        prefs.playerPath = v.optStr("playerPath");
    }

    void SaveUiPrefs() {
        JVal v = JVal::obj();
        v.set("theme", JVal(prefs.theme));
        v.set("colWidths", prefs.colWidths);
        v.set("sort", prefs.sort);
        v.set("window", prefs.window);
        v.set("weaponFilter", JVal(prefs.weaponFilter));
        v.set("histExpanded", JVal(prefs.histExpanded));
        v.set("onlyActive", JVal(prefs.onlyActive));
        v.set("wemView", JVal(prefs.wemView));
        v.set("playerPath", JVal(prefs.playerPath));
        FsWrite(uiPath, JsonDump(v));
        uiDirty = false;
    }

    JVal UiPrefsJson() const {
        JVal v = JVal::obj();
        v.set("theme", JVal(prefs.theme));
        v.set("colWidths", prefs.colWidths);
        v.set("sort", prefs.sort);
        v.set("window", prefs.window);
        v.set("weaponFilter", JVal(prefs.weaponFilter));
        v.set("histExpanded", JVal(prefs.histExpanded));
        v.set("onlyActive", JVal(prefs.onlyActive));
        v.set("wemView", JVal(prefs.wemView));
        v.set("playerPath", JVal(prefs.playerPath));
        return v;
    }

    // ---- 实时捕获 ----
    // ---- wem 捕获通道（读 SonarAudio.log 的增量，单独走，不需要游戏内插件）----
    void InitWemWatch() {
        std::vector<std::string> cand;
        const std::size_t pl = gameIniPath.find("nativePC\\plugins\\");
        if (pl != std::string::npos)
            cand.push_back(gameIniPath.substr(0, pl + std::string("nativePC\\plugins\\").size()));
        cand.push_back(exeDir + "..\\");
        wemSeqPath = exeDir + "WseMediaSeqs.txt";     // 真值表随 GUI 部署，优先 exe 目录
        if (!FsExists(wemSeqPath) && !cand.empty())
            wemSeqPath = cand[0] + "WseMediaSeqs.txt";

        // media_id → 可读名映射表：优先 GUI 目录（随包分发，删不掉），
        // 其次 plugins 根（老布局）。以前只在 plugins 根找，文件一被清理名字就全空了。
        wemMapPath.clear();
        if (FsExists(exeDir + "SonarAudio.mediaids.txt")) {
            wemMapPath = exeDir + "SonarAudio.mediaids.txt";
        } else {
            for (const auto& d : cand) {
                if (FsExists(d + "SonarAudio.mediaids.txt")) {
                    wemMapPath = d + "SonarAudio.mediaids.txt";
                    break;
                }
            }
        }
        // 特效捕获日志（同一个 plugins 目录；DLL 的 sonarefx 模块写）
        for (const auto& d : cand) {
            if (FsExists(d + "WeaponSoundEnhance_efx.log")) { efxLogPath = d + "WeaponSoundEnhance_efx.log"; break; }
        }
        if (efxLogPath.empty() && !cand.empty()) efxLogPath = cand[0] + "WeaponSoundEnhance_efx.log";
        for (const auto& d : cand) {
            // 新名（我们自己的 DLL 写）；旧名 SonarAudio.log 兼容老版本
            if (FsExists(d + "WeaponSoundEnhance_wem.log")) {
                wemLogPath = d + "WeaponSoundEnhance_wem.log";
                return;
            }
            if (FsExists(d + "SonarAudio.log")) {
                wemLogPath = d + "SonarAudio.log";
                return;
            }
        }
        if (!cand.empty()) {   // log 还没生成（游戏没跑过）也先占位，运行时照常尝试
            wemLogPath = cand[0] + "WeaponSoundEnhance_wem.log";
        }
    }

    // 用户自定义 wem 名字库：读入并覆盖官方映射（用户优先）
    void LoadWemUserNames() {
        wemUserNames.clear();
        if (wemUserNamePath.empty()) return;
        std::ifstream f(wemUserNamePath, std::ios::binary);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const std::size_t t = line.find('\t');
            if (t == std::string::npos) continue;
            const int id = std::atoi(line.substr(0, t).c_str());
            const std::string nm = Trim(line.substr(t + 1));
            if (id > 0 && !nm.empty()) {
                wemUserNames[id] = nm;
                wemNames[id] = nm;          // ★ 用户库优先于官方映射
            }
        }
        if (!wemUserNames.empty()) hasWemNames = true;
    }

    bool SaveWemUserNames(std::string& err) {
        if (wemUserNamePath.empty()) { err = "用户名字库路径未知"; return false; }
        std::string out = "# Sonar 用户自定义 wem 名字库（media_id <TAB> 名字）\r\n";
        out += "# 由 GUI 维护；导入 nbnk / 重启 GUI 时自动读取。删掉本文件即恢复官方名字。\r\n";
        std::vector<std::pair<int, std::string> > v(wemUserNames.begin(), wemUserNames.end());
        std::sort(v.begin(), v.end());
        for (std::size_t i = 0; i < v.size(); ++i)
            out += std::to_string(v[i].first) + "\t" + v[i].second + "\r\n";
        if (!FsWrite(wemUserNamePath, out)) { err = "无法写入用户名字库"; return false; }
        return true;
    }

    // 删除/修改后重建映射：官方库 + 用户库
    void ReloadWemNames() {
        LoadWemMap();
        LoadWemUserNames();
    }

    // ---- 内嵌资源（武器图标 / logo / 成就音效）解压到 %LOCALAPPDATA%\Sonar\assets ----
    //      资源以 zip 形式编进 exe（RCDATA #200），界面通过 https://sonar.assets 访问，
    //      所以游戏目录里不再出现 weapons_icons\ / sonar_icon.png / coinmul.wav。
    // ---- 更新检查辅助：时间比较（版本号没变也能发现热修复包）----
    // "2026-10-03T18:32:52Z" → Unix 秒（UTC）；解析失败返回 0
    static unsigned long long ParseIsoUtc(const std::string& s) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
        if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
        if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
        SYSTEMTIME st{};
        st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
        st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)se;
        FILETIME ft{};
        if (!::SystemTimeToFileTime(&st, &ft)) return 0;
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        return u.QuadPart / 10000000ULL - 11644473600ULL;   // FILETIME → Unix 秒
    }
    // 本地 exe（配置工具自身）的写入时间 → Unix 秒（UTC）
    static unsigned long long ExeWriteTimeUtc() {
        wchar_t p[MAX_PATH * 2] = {};
        if (!::GetModuleFileNameW(nullptr, p, MAX_PATH * 2)) return 0;
        HANDLE h = ::CreateFileW(p, GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return 0;
        FILETIME ft{};
        const BOOL ok = ::GetFileTime(h, nullptr, nullptr, &ft);
        ::CloseHandle(h);
        if (!ok) return 0;
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        return u.QuadPart / 10000000ULL - 11644473600ULL;
    }

    static std::string EnvDirFallback() {
        char tmp[MAX_PATH] = {};
        ::GetTempPathA(MAX_PATH, tmp);
        return std::string(tmp);
    }
    static std::string LocalAssetsDir() {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
        std::string base = (n > 0) ? Utf8FromWide(buf) : EnvDirFallback();
        std::string dir = base + "\\Sonar\\assets";
        return dir;
    }

    void ExtractEmbeddedAssets() {
        HMODULE mod = ::GetModuleHandleW(nullptr);
        HRSRC res = ::FindResourceW(mod, MAKEINTRESOURCEW(200), RT_RCDATA);
        if (!res) return;
        const DWORD size = ::SizeofResource(mod, res);
        HGLOBAL hg = ::LoadResource(mod, res);
        if (!hg || size == 0) return;
        const void* data = ::LockResource(hg);
        if (!data) return;

        assetsDir = LocalAssetsDir();
        const std::string marker = assetsDir + "\\.assets_ver";
        char want[64] = {};
        _snprintf_s(want, _TRUNCATE, "%lu", (unsigned long)size);
        {
            std::string have;
            if (FsRead(marker, have) && have == want) return;   // 已是最新，跳过
        }
        MakeDirs(Utf8ToWide(assetsDir));

        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, data, (size_t)size, 0)) return;
        const mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n; ++i) {
            char name[512] = {};
            mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
            std::string rel(name);
            for (std::size_t k = 0; k < rel.size(); ++k) if (rel[k] == '\\') rel[k] = '/';
            if (rel.empty() || rel.back() == '/') continue;
            // 目录用相对名字建（miniz 的 extract_to_file 收 ANSI 路径，文件名都是 ASCII）
            std::string relDir = rel;
            const std::size_t rs = relDir.find_last_of('/');
            std::string sub = assetsDir;
            if (rs != std::string::npos) {
                relDir = relDir.substr(0, rs);
                for (std::size_t k = 0; k < relDir.size(); ++k) if (relDir[k] == '/') relDir[k] = '\\';
                sub = assetsDir + "\\" + relDir;
            }
            MakeDirs(Utf8ToWide(sub));
            std::string relWin = rel;
            for (std::size_t k = 0; k < relWin.size(); ++k) if (relWin[k] == '/') relWin[k] = '\\';
            const std::string fullAnsi = assetsDir + "\\" + relWin;
            mz_zip_reader_extract_to_file(&zip, i, fullAnsi.c_str(), 0);
        }
        mz_zip_reader_end(&zip);
        FsWrite(marker, want);
    }

    // 播放内嵌资源里的音频（成就音效 coinmul.wav 等）
    bool PlayAsset(const std::string& name) {
        if (assetsDir.empty()) return false;
        const std::string full = assetsDir + "\\" + name;
        const std::wstring w = Utf8ToWide(full);
        if (::GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        ::PlaySoundW(nullptr, nullptr, 0);
        return ::PlaySoundW(w.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT) != FALSE;
    }

    // mediaids 的值形如 "wp_bow_cmn/01.ogg （瓶子声）"：提取括号里的中文注释当"名字"。
    // （用户要求名称列只显示这种可读名，不要 bank/NN.ext 这种文件名形态）
    static std::string ExtractNote(const std::string& val) {
        // 全角 （） 优先（3 字节），其次半角 ()（1 字节）
        std::size_t a = val.find("（");
        std::size_t openLen = 3;
        std::string closeStr = "）";
        if (a == std::string::npos) {
            a = val.find('(');
            openLen = 1;
            closeStr = ")";
        }
        if (a == std::string::npos) return std::string();
        const std::size_t c = val.find(closeStr, a + openLen);
        std::string note = (c == std::string::npos) ? val.substr(a + openLen)
                                                    : val.substr(a + openLen, c - (a + openLen));
        return Trim(note);
    }

    void LoadWemMap() {
        wemNames.clear();
        wemNotes.clear();
        hasWemNames = false;
        if (wemMapPath.empty()) return;
        std::ifstream f(wemMapPath, std::ios::binary);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const int id = std::atoi(line.substr(0, tab).c_str());
            const std::string nm = line.substr(tab + 1);
            if (id > 0 && !nm.empty()) {
                wemNames[id] = nm;
                const std::string note = ExtractNote(nm);
                if (!note.empty()) wemNotes[id] = note;
            }
        }
        hasWemNames = !wemNames.empty();
    }

    // 加载 media_id → (bank, DIDX 序号) 真值表（离线全扫 nbnk 生成）
    void LoadWemSeqs() {
        wemSeqs.clear();
        if (wemSeqPath.empty()) return;
        std::ifstream f(wemSeqPath, std::ios::binary);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            std::size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            std::size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            const int id = std::atoi(line.substr(0, t1).c_str());
            const std::string bank = line.substr(t1 + 1, t2 - t1 - 1);
            const int seq = std::atoi(line.c_str() + t2 + 1);
            if (id > 0 && !bank.empty() && seq > 0) wemSeqs[id] = { bank, seq };
        }
    }

    // 从 "bank/NN.ext" 解析出 bank 与序号（agent 的 name= 或 mediaids 名）
    bool ParseSeqFromName(const std::string& nm, std::string& bankOut, int& numOut) const {
        const std::size_t slash = nm.find_last_of("/\\");
        if (slash == std::string::npos) return false;
        std::size_t i = slash + 1;
        int num = 0;
        bool hasDigit = false;
        while (i < nm.size() && nm[i] >= '0' && nm[i] <= '9') { num = num * 10 + (nm[i] - '0'); ++i; hasDigit = true; }
        if (!hasDigit || num <= 0) return false;
        bankOut = nm.substr(0, slash);
        numOut = num;
        return true;
    }

    // 给条目 JSON 附 wem 序号（media id → bank/第N 个），显示不再依赖文件名
    void AttachSeq(JVal& v, long long media) const {
        if (media <= 0) return;
        const auto it = wemSeqs.find((int)media);
        if (it != wemSeqs.end()) {
            v.set("seqNum", JVal(it->second.second));
            if (!v.find("seqBank")) v.set("seqBank", JVal(it->second.first));
            if (!v.find("bank")) v.set("bank", JVal(it->second.first));
            return;
        }
        // 真值表没有：从名字兜底（agent 的 name=bank/NN.wem、mediaids 的 bank/NN.ogg）
        const std::string nm = v.optStr("name");
        const std::size_t slash = nm.find_last_of("/\\");
        if (slash == std::string::npos) return;
        const std::string head = nm.substr(0, slash);
        std::size_t i = slash + 1;
        int num = 0;
        bool hasDigit = false;
        while (i < nm.size() && nm[i] >= '0' && nm[i] <= '9') { num = num * 10 + (nm[i] - '0'); ++i; hasDigit = true; }
        if (!hasDigit || num <= 0) return;
        v.set("seqNum", JVal(num));
        if (!v.find("seqBank")) v.set("seqBank", JVal(head));
        if (!v.find("bank")) v.set("bank", JVal(head));
    }

    // bank（nbnk 名）→ 武器类型。数字前缀 wp00..wp13 直接取值（11=弓箭 12=重弩 13=轻弩）；
    // 命名 bank 用对照表（源自 D:\下载\音效源文件（含笔记） 的目录结构）。
    int WemWeapon(const std::string& bank) const {
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

    // 解析 SonarAudio.log 的事件行：
    //   evt seq#.. id=.. playing=.. gobj=.. media=<真 media id> wem=<size> name=<bank/NN.wem> bank=.. path=..
    //   ★ media= 才是 media id；wem= 是 wem 字节大小（不是 id，不能当键）
    bool ParseWemLine(const std::string& line, int& media, std::string& bank, std::string& path,
                     std::string* nameOut = nullptr) const {
        if (line.find("media=") == std::string::npos) return false;
        auto grab = [&](const char* key, std::string& dst) {
            const std::string k(key);
            std::size_t p = line.find(k);
            if (p == std::string::npos) return;
            p += k.size();
            std::size_t en = p;
            while (en < line.size() && line[en] != ' ' && line[en] != '\t' && line[en] != '\r') ++en;
            dst = line.substr(p, en - p);
        };
        std::string ms;
        grab("media=", ms);
        if (ms.empty()) return false;
        const long long m = std::strtoll(ms.c_str(), nullptr, 10);
        if (m <= 0) return false;
        media = (int)m;
        grab("bank=", bank);
        grab("path=", path);
        if (nameOut) { nameOut->clear(); grab("name=", *nameOut); }
        return true;
    }

    // 每 ~250ms 增量扫一遍 SonarAudio.log，把新 wem 事件收进捕获历史
    void RecordWemEvents() {
        const unsigned long long nowMs = ::GetTickCount64();
        if (nowMs - wemLastPoll < 250) return;
        wemLastPoll = nowMs;
        if (wemLogPath.empty()) return;
        if (wemStream.is_open() && wemStream.eof()) wemStream.clear();
        if (!wemStream.is_open()) {
            wemStream.open(wemLogPath, std::ios::binary);
            // ★ 打开后直接跳到文件末尾：不回放历史。
            //   以前从头读 → 启动瞬间灌进整份日志（"一开始就有很多历史"），
            //   还会把循环音判定表填满，导致真实事件被静默（"捕获不到"）。
            if (wemStream.is_open()) wemStream.seekg(0, std::ios::end);
        }
        if (!wemStream.is_open()) return;
        std::string line;
        while (std::getline(wemStream, line)) {
            if (line.size() < 8) continue;
            int media = 0;
            std::string bank, path, logName;
            if (!ParseWemLine(line, media, bank, path, &logName)) continue;
            if (media <= 0) { ++wemZeroCount; continue; }   // 反查不到 bank 的事件（界面会显示统计）
            // 循环音过滤：有些 wem 是持久循环音（例 wp_bow_cmn/10 = 182089195，
            // 约每秒一次、持续整场），不是动作事件，会把捕获历史刷屏。
            // 判据用行为（wem 数据里没有 loop 标志）：4 秒内出现 >= 4 次 → 判为循环音，
            // 随后 8 秒内直接忽略该 media（真动作不可能 4 秒内重复 4 次同一 wem）。
            {
                std::unordered_map<int, LoopStat>::iterator lit = wemLoops.find(media);
                if (lit == wemLoops.end()) {
                    LoopStat st; st.winStart = nowMs; st.count = 1; st.muteUntil = 0;
                    wemLoops[media] = st;
                } else {
                    if (nowMs < lit->second.muteUntil) { ++wemLoopDrop; continue; }   // 循环音静默
                    if (nowMs - lit->second.winStart > 3000) {
                        lit->second.winStart = nowMs;
                        lit->second.count = 0;
                    }
                    if (++lit->second.count >= 6) {
                        lit->second.muteUntil = nowMs + 4000;
                        continue;                                     // 判为循环音，不记录
                    }
                }
                if (wemLoops.size() > 2048) wemLoops.clear();
            }
            // 去重：连续同 media，或 1.5s 窗口内同 media
            bool dup = false;
            if (!wemHist.empty()) {
                const HistEntry& top = wemHist.front();
                if (top.wemMedia == media) dup = true;
            }
            if (!dup) {
                const unsigned long long win = 1500;
                for (std::size_t i = 0; i < wemHist.size(); ++i) {
                    const HistEntry& ph = wemHist[i];
                    if (nowMs - ph.ms >= win) break;
                    if (ph.wemMedia == media) { dup = true; break; }
                }
            }
            if (dup) { ++wemDup; continue; }
            std::string name;
            const std::unordered_map<int, std::string>::const_iterator it = wemNames.find(media);
            if (it != wemNames.end()) name = it->second;         // 形如 wp_bow_cmn/30.ogg
            else if (!logName.empty()) name = logName;           // agent 的 name=（bank/NN.wem）
            else if (!bank.empty()) name = bank;                 // 纯 nbnk 文件名，绝不用本地路径
            else name = "media " + std::to_string(media);
            HistEntry h;
            h.kind = 1;
            h.wemMedia = media;
            h.wemName = name;
            h.bank = bank;
            h.weapon = WemWeapon(bank);
            h.weaponId = -1;
            h.ms = nowMs;
            h.time = TimeNowHms();
            wemHist.insert(wemHist.begin(), h);
            if (wemHist.size() > 300) wemHist.pop_back();
            ++wemOkCount;
            ++wemRev;
            curWemMedia = media;
            curWemName = name;
            curWemBank = bank;
            curWemWeapon = h.weapon;
        }
        wemStream.clear();
        wemStream.seekg(0, std::ios::end);   // 始终从尾部续读（文件被游戏重建/截断时也稳）
    }

    void RecordHistory() {
        if (!(liveOk && live.attached) || !live.inScene) return;
        if (live.fsm == -1) return;
        const unsigned long long nowMs = ::GetTickCount64();
        // 去重分两层，必须都有：
        //
        // ① 「和最新一条完全相同」→ 永不重复记录（不看时间）。
        //    轮询间隔可能接近 1 秒（PollMs 配置），光靠时间窗覆盖不住，
        //    站着不动时会打出一长串一模一样的 fsm/lmt。
        // ② 时间窗内（1.5s）扫描最近记录，同 id 也跳过。
        //    一次动作里 fsm 可能 92→90→92 地来回跳，只看最新一条会让 92 出现两次。
        //
        // 【顺序】历史最新在头部（insert begin）：① 比 front，② 从头往后扫。
        if (!derHist.empty()) {
            const HistEntry& top = derHist.front();
            if (top.fsm == live.fsm && top.lmt == live.lmt && top.weapon == live.weapon) { ++derDup; return; }
        }
        const unsigned long long win = 3000;   // 派生：3 秒内同一 fsm/lmt/weapon 只记一条
        for (std::size_t i = 0; i < derHist.size(); ++i) {
            const HistEntry& ph = derHist[i];
            if (nowMs - ph.ms >= win) break;
            if (ph.fsm == live.fsm && ph.lmt == live.lmt && ph.weapon == live.weapon) { ++derDup; return; }
        }
        HistEntry h;
        h.ms = nowMs;
        h.fsm = live.fsm;
        h.lmt = live.lmt;
        h.weapon = live.weapon;
        h.weaponId = live.weaponId;
        h.time = TimeNowHms();
        derHist.insert(derHist.begin(), h);
        if (derHist.size() > 300) derHist.pop_back();                   // 只留最近 300 条
        ++derRev;
        // 最新记录放最前面：捕获面板从上往下就是"新 → 旧"，不用翻到底找刚做的动作
    }

    void PollGame() {
        const unsigned long long now = ::GetTickCount64();
        // 采样间隔跟随 ini 的 PollMs（插件触发用的同一个值，默认 60ms）。
        // 250ms 会漏掉弓箭平射这类"一帧就结束"的短动作。
        unsigned long interval = 250;
        if (cfg.global.pollMs >= 20) interval = (unsigned long)cfg.global.pollMs;
        if (now - lastPoll < interval) return;
        lastPoll = now;

        const std::uint64_t pr = ParsePlayerRoot(cfg.global.playerRoot, 0x1450139A0ULL);
        // 传入 PlayerRoot：Attach 会逐个候选进程验证指针链，避免连到残留的僵尸进程
        if (!game.IsAttached()) game.Attach(pr);
        if (!game.IsAttached()) {
            liveOk = false;
            live = LiveState();
            live.error = game.LastError();
            return;
        }
        liveOk = game.Poll(pr, live);
        if (!game.IsAttached()) liveOk = false;
        RecordWemEvents();     // wem 捕获（读 SonarAudio.log；不依赖进场景/附着）
        RecordEfxEvents();    // 特效捕获（读 WeaponSoundEnhance_efx.log）
        RecordHistory();
    }

    // ================= wem 播放：任何捕获到的 media 都能播 =================
    //   反查顺序：① 真值表 media→bank 名 → 候选目录找 <bank>.nbnk
    //             ② 调用方显式给的 bank 名   ③ 全量 DIDX 索引兜底（首次扫一遍，之后缓存）
    std::vector<std::string> bankSearchDirs;
    std::unordered_map<std::string, std::string> bankPathCache;   // bank 名 → 路径（含未命中的空串）
    std::unordered_map<int, std::string> mediaBankIndex;          // media → bank 路径（兜底索引）
    bool mediaBankIndexBuilt = false;

    static std::string FindFileRecursive(const std::wstring& dir, const std::wstring& name, int depth) {
        if (depth < 0) return std::string();
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return std::string();
        std::string hit;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.cFileName[0] == L'.') continue;
                hit = FindFileRecursive(dir + L"\\" + fd.cFileName, name, depth - 1);
                if (!hit.empty()) break;
            } else if (::_wcsicmp(fd.cFileName, name.c_str()) == 0) {
                hit = Utf8FromWide(dir + L"\\" + fd.cFileName);
                break;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        return hit;
    }

    static void CollectNbnk(const std::wstring& dir, std::vector<std::wstring>& out, int depth) {
        if (depth < 0) return;
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.cFileName[0] == L'.') continue;
                CollectNbnk(dir + L"\\" + fd.cFileName, out, depth - 1);
            } else {
                const std::wstring n = fd.cFileName;
                if (n.size() > 5 && ::_wcsicmp(n.c_str() + n.size() - 5, L".nbnk") == 0)
                    out.push_back(dir + L"\\" + n);
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }

    // 只读 DIDX（读到 DATA 就停），用于快速建立 media→bank 索引
    static bool ScanBankDIDX(const std::wstring& path, std::vector<uint32_t>& ids) {
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        char hdr[8] = {};
        DWORD got = 0;
        bool ok = false;
        while (::ReadFile(h, hdr, 8, &got, nullptr) && got == 8) {
            const uint32_t len = *(const uint32_t*)(hdr + 4);
            if (std::memcmp(hdr, "DIDX", 4) == 0) {
                if (len >= 12 && len <= (64u << 20)) {
                    std::vector<char> buf(len);
                    if (::ReadFile(h, buf.data(), len, &got, nullptr) && got == len) {
                        const uint32_t n = len / 12;
                        for (uint32_t i = 0; i < n; ++i)
                            ids.push_back(*(const uint32_t*)(buf.data() + (std::size_t)i * 12));
                        ok = true;
                    }
                }
                break;
            }
            if (len == 0) break;
            if (::SetFilePointer(h, (LONG)len, nullptr, FILE_CURRENT) == INVALID_SET_FILE_POINTER) break;
        }
        ::CloseHandle(h);
        return ok;
    }

    // 候选目录：与插件（SonarAudio）扫描 bank 的那份列表保持一致，
    // 否则会出现"插件能反查到、GUI 却找不到文件"的尴尬。
    void BuildBankSearchDirs() {
        if (!bankSearchDirs.empty()) return;
        struct Adder {
            std::vector<std::string>* v;
            void operator()(const std::string& raw) const {
                std::string d = raw;
                while (!d.empty() && (d[d.size() - 1] == '\\' || d[d.size() - 1] == '/')) d.erase(d.size() - 1);
                if (d.empty() || !FsExists(d)) return;
                for (std::size_t i = 0; i < v->size(); ++i) if ((*v)[i] == d) return;
                v->push_back(d);
            }
        } add;
        add.v = &bankSearchDirs;

        // (0) 插件 ini 的 BankRoot（分号分隔，优先级最高）
        {
            std::string ini;
            if (FsRead(exeDir + "..\\WeaponSoundEnhance.wem.ini", ini)) {
                std::size_t p0 = 0;
                while (p0 <= ini.size()) {
                    std::size_t e = ini.find('\n', p0);
                    if (e == std::string::npos) e = ini.size();
                    const std::string line = Trim(ini.substr(p0, e - p0));
                    p0 = e + 1;
                    if (line.size() > 9 && _strnicmp(line.c_str(), "BankRoot=", 9) == 0) {
                        const std::string v = line.substr(9);
                        std::size_t q = 0;
                        while (q <= v.size()) {
                            std::size_t s = v.find(';', q);
                            if (s == std::string::npos) s = v.size();
                            add(Trim(v.substr(q, s - q)));
                            q = s + 1;
                        }
                    }
                    if (e == ini.size()) break;
                }
            }
        }

        // 游戏根：GUI 在 <游戏>\nativePC\plugins\WeaponSoundEnhance\ → 往上 3 级
        std::string game;
        {
            std::string d = exeDir;
            while (!d.empty() && (d[d.size() - 1] == '\\' || d[d.size() - 1] == '/')) d.erase(d.size() - 1);
            for (int i = 0; i < 3; ++i) {
                const std::size_t s = d.find_last_of("\\/");
                if (s == std::string::npos) { d.clear(); break; }
                d = d.substr(0, s);
            }
            game = d;
        }

        // (1) 插件目录（有人把 bank 直接放 plugins 下）
        add(exeDir);
        add(exeDir + "..");
        // (2)(3)(4) 游戏 nativePC / 游戏根 / 解包出来的 wwise 目录
        if (!game.empty()) {
            add(game + "\\nativePC");
            add(game + "\\nativePC\\sound\\wwise\\Windows");
            add(game + "\\nativePC\\sound\\wwise\\Windows\\Japanese");
            add(game);
            add(game + "\\chunk\\sound\\wwise\\Windows");
            add(game + "\\mhwmod解包\\chunk\\sound\\wwise\\Windows");
        }
        // (5) 本机常见解包位置（不存在会自动跳过，与插件侧一致）
        add("D:\\下载\\音效源文件（含笔记）");
        add("D:\\mhwmod解包\\chunk\\sound\\wwise\\Windows");
        add("D:\\mhwmod解包\\chunkG8\\sound\\wwise\\Windows");
        add("D:\\mhwmod解包\\sound\\wwise\\Windows");
        add("D:\\mhwmod解包\\stamp");
        // (6) 用户设定的 nbnk 导出目录
        add(cfg.global.bankOutDir);
    }

    std::string FindBankByName(const std::string& bank) {
        if (bank.empty()) return std::string();
        const std::unordered_map<std::string, std::string>::const_iterator ci = bankPathCache.find(bank);
        if (ci != bankPathCache.end()) return ci->second;
        BuildBankSearchDirs();
        const std::wstring want = Utf8ToWide(bank + ".nbnk");
        std::string found;
        for (std::size_t i = 0; i < bankSearchDirs.size() && found.empty(); ++i) {
            const std::string direct = bankSearchDirs[i] + "\\" + bank + ".nbnk";
            if (FsExists(direct)) { found = direct; break; }
            found = FindFileRecursive(Utf8ToWide(bankSearchDirs[i]), want, 4);
        }
        bankPathCache[bank] = found;
        return found;
    }

    void BuildMediaBankIndex() {
        if (mediaBankIndexBuilt) return;
        mediaBankIndexBuilt = true;
        BuildBankSearchDirs();
        std::vector<std::wstring> files;
        for (std::size_t i = 0; i < bankSearchDirs.size(); ++i)
            CollectNbnk(Utf8ToWide(bankSearchDirs[i]), files, 4);
        for (std::size_t i = 0; i < files.size(); ++i) {
            std::vector<uint32_t> ids;
            if (!ScanBankDIDX(files[i], ids)) continue;
            const std::string p = Utf8FromWide(files[i]);
            for (std::size_t k = 0; k < ids.size(); ++k) {
                const int id = (int)ids[k];
                const std::unordered_map<int, std::string>::iterator it = mediaBankIndex.find(id);
                if (it == mediaBankIndex.end()) { mediaBankIndex[id] = p; continue; }
                // 优先保留游戏 nativePC 里的那份（游戏真正加载的），避免播出解包原版/
                // mod 前版本的音效（用户报过"播放的是无关音效"）
                const bool oldNative = it->second.find("nativePC") != std::string::npos;
                const bool newNative = p.find("nativePC") != std::string::npos;
                if (!oldNative && newNative) it->second = p;
            }
        }
    }

    // wem 字节 → vgmstream 解码到内存 → PlaySound（GUI 内播放，不调外部播放器）
    bool PlayWemBytes(const std::vector<uint8_t>& wem, long long media, std::string& err) {
        std::wstring cli = Utf8ToWide(exeDir) + L"wemkit\\vgmstream\\vgmstream-cli.exe";
        if (::GetFileAttributesW(cli.c_str()) == INVALID_FILE_ATTRIBUTES)
            cli = Utf8ToWide(exeDir) + L"tools\\vgmstream\\vgmstream-cli.exe";
        if (::GetFileAttributesW(cli.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = "缺少 wemkit\\vgmstream\\vgmstream-cli.exe（随包分发的 wem 解码器）";
            return false;
        }
        const std::wstring tmpDir = Utf8ToWide(exeDir) + L"wemkit\\tmp";
        ::CreateDirectoryW(tmpDir.c_str(), nullptr);
        wchar_t num[32] = {};
        ::swprintf_s(num, L"%lld", media);
        const std::wstring wemFile = tmpDir + L"\\play_" + num + L".wem";
        {
            HANDLE h = ::CreateFileW(wemFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) { err = "无法写临时 wem"; return false; }
            DWORD wr0 = 0;
            ::WriteFile(h, wem.data(), (DWORD)wem.size(), &wr0, nullptr);
            ::CloseHandle(h);
        }
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE rd = nullptr, wr = nullptr;
        if (!::CreatePipe(&rd, &wr, &sa, 1 << 20)) { err = "创建管道失败"; return false; }
        ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = wr;
        si.hStdError = wr;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + cli + L"\" -p \"" + wemFile + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        const BOOL ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                         CREATE_NO_WINDOW, nullptr, tmpDir.c_str(), &si, &pi);
        ::CloseHandle(wr);
        std::vector<unsigned char> wav;
        if (ok) {
            ::CloseHandle(pi.hThread);
            unsigned char chunk[65536];
            DWORD got = 0;
            while (::ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) && got > 0)
                wav.insert(wav.end(), chunk, chunk + got);
            ::WaitForSingleObject(pi.hProcess, 15000);
            ::CloseHandle(pi.hProcess);
        }
        ::CloseHandle(rd);
        if (wav.size() < 44) { err = "wem 解码失败（vgmstream 没有输出 wav）"; return false; }
        ::PlaySoundW(nullptr, nullptr, 0);
        playBuf.swap(wav);
        if (!::PlaySoundW((LPCWSTR)playBuf.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT)) {
            err = "PlaySound 失败";
            return false;
        }
        return true;
    }

    // 统一的 wem 显示名：用户命名 > 官方注释名 > bank·第N个 > 日志里的名字 > media N。
    // 列表行和"正在播放"行都必须走这里，否则同一条 media 在两处显示不同名字
    // （用户会以为"播放的和我点的不一样"）。
    std::string WemDispName(int media, const std::string& logName, const std::string& bankIn) const {
        const std::unordered_map<int, std::string>::const_iterator uit = wemUserNames.find(media);
        if (uit != wemUserNames.end() && !uit->second.empty()) return uit->second;
        const std::unordered_map<int, std::string>::const_iterator nit = wemNotes.find(media);
        if (nit != wemNotes.end() && !nit->second.empty()) return nit->second;
        std::string bank = bankIn;
        int seq = 0;
        const std::unordered_map<int, std::pair<std::string, int>>::const_iterator sit = wemSeqs.find(media);
        if (sit != wemSeqs.end()) {
            if (bank.empty()) bank = sit->second.first;
            seq = sit->second.second;
        } else {
            std::string b; int n = 0;
            if (ParseSeqFromName(logName, b, n)) { if (bank.empty()) bank = b; seq = n; }
        }
        if (!bank.empty()) return seq > 0 ? (bank + " · 第" + std::to_string(seq) + "个") : bank;
        if (!logName.empty()) return logName;
        return "media " + std::to_string(media);
    }

    // 派生捕获列表（独立）
    JVal DerJson() const {
        JVal a = JVal::arr();
        for (std::size_t i = 0; i < derHist.size(); ++i) {
            const HistEntry& e = derHist[i];
            JVal h = JVal::obj();
            h.set("kind", JVal(0));
            h.set("fsm", JVal(e.fsm));
            h.set("lmt", JVal(e.lmt));
            h.set("weapon", JVal(e.weapon));
            h.set("weaponId", JVal(e.weaponId));
            h.set("time", JVal(e.time));
            h.set("ageMs", JVal((long long)(::GetTickCount64() - e.ms)));
            h.set("name", JVal(ResolveName(e.weapon, e.fsm, e.lmt)));
            h.set("added", JVal(IsCapturedAdded(e.weapon, e.fsm, e.lmt)));
            h.set("matches", JVal(CapturedMatchCount(e.weapon, e.fsm, e.lmt)));   // 同一触发条件有几条条目
            a.push(h);
        }
        return a;
    }

    // wem 捕获列表（独立）
    // ---- 特效捕获（实验性）：读 plugins\WeaponSoundEnhance_efx.log ----
    //   日志由 DLL 的 sonarefx 模块写：hook 引擎的 Effect:Ctor，只记落在 hm_wp* 表里的调用。
    //   行格式：efx seq#N table=hm_wp03 group=2005 rec=2 id=-1 weapon=3 name=vfx\...
    //   头部还有一行 # sonarefx hook=ok ... / hook 失败原因（界面直接显示，不静默）
    // "# sonarefx hook=ok thunks=N [T0@0xAE0090,...] tables=X entries=Y sig=..." → 给界面看的中文摘要
    static std::string EfxHumanStatus(const std::string& s) {
        auto num = [&s](const char* key) -> std::string {
            const std::string k(key);
            const std::size_t a = s.find(k);
            if (a == std::string::npos) return std::string();
            const std::size_t b = a + k.size();
            std::size_t e = b;
            while (e < s.size() && s[e] != ' ' && s[e] != ',') ++e;
            return s.substr(b, e - b);
        };
        const std::string thunks = num("thunks=");
        const std::string tables = num("tables=");
        const std::string entries = num("entries=");
        std::string out = "挂钩成功";
        if (!thunks.empty()) out += "：" + thunks + " 条特效跳板";
        if (!tables.empty()) out += " · 索引 " + tables + " 表 / " + entries + " 条";
        return out;
    }

    // ---- 特效捕获（实验性）：读 plugins\WeaponSoundEnhance_efx.log ----
    //   日志由 DLL 的 sonarefx 模块写：挂钩引擎的特效生成函数，只记落在 hm_wp* 表里的调用。
    //   行格式：efx seq#N via=Tn table=hm_wp03 group=2005 rec=2 id=-1 weapon=3 name=vfx\...
    //   头部/定期还有 # sonarefx status:… / index:… / # stats …（界面直接显示，不静默）
    void RecordEfxEvents() {
        if (cfg.global.efxCapture == 0) return;   // 模块已关闭：不读日志（零开销）
        // ★ 文件被删/被清空时自动重开：Windows 下"文件删除后旧句柄仍指向旧数据"，
        //   界面会永远读不到新内容（实测：清空日志后 GUI 一直显示"一个都捕获不到"）。
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            const bool exists = ::GetFileAttributesExW(Utf8ToWide(efxLogPath).c_str(),
                                                       GetFileExInfoStandard, &fad) != FALSE;
            const unsigned long long sz = exists
                ? (((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow) : 0ULL;
            if (!exists || sz < efxLogPos) {
                if (efxStream.is_open()) efxStream.close();
                efxLogPos = 0;
            }
        }

        const unsigned long long nowMs = ::GetTickCount64();
        if (nowMs - efxLastPoll < 250) return;
        efxLastPoll = nowMs;
        if (efxLogPath.empty()) return;
        if (efxStream.is_open() && efxStream.eof()) efxStream.clear();
        if (!efxStream.is_open()) {
            efxStream.open(efxLogPath, std::ios::binary);
            if (efxStream.is_open()) {
                // 先把文件**开头**的自检行读掉（hook 结果 / 索引来源）：日志大了以后
                // 那些行会落在"尾部 64KB"窗口之外，界面就一直空着（实测问题）。
                std::ifstream head(efxLogPath, std::ios::binary);
                std::string l1;
                for (int k = 0; k < 6 && std::getline(head, l1); ++k) {
                    if (!l1.empty() && l1.back() == '\r') l1.pop_back();
                    if (l1.compare(0, 11, "# sonarefx ") != 0) continue;
                    const std::string s = l1.substr(11);
                    if (s.compare(0, 6, "index:") == 0) { efxIndexInfo = s.substr(7); continue; }
                    if (s.compare(0, 7, "status:") == 0) { efxStatus = s.substr(8); continue; }
                    if (s.compare(0, 8, "hook=ok ") == 0) efxStatus = EfxHumanStatus(s);
                }
                // 首次打开读**日志尾部**（最近 64KB）：用户通常是"先打一会儿、再打开 GUI"，
                // 跳到末尾会让已有记录全被跳过（会被误判成"没捕获到"）。
                efxStream.seekg(0, std::ios::end);
                const std::streamoff endPos = efxStream.tellg();
                if (endPos > 65536) {
                    efxStream.seekg(endPos - 65536);
                    std::string junk;
                    std::getline(efxStream, junk);   // 丢掉被截断的半行
                } else {
                    efxStream.seekg(0);
                }
            }
        }
        if (!efxStream.is_open()) return;

        std::string line;
        while (std::getline(efxStream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line[0] == '#') {                      // 自检/统计行：记下来给界面显示
                if (line.compare(0, 8, "# stats ") == 0) { efxStats = line.substr(8); continue; }
                const std::size_t p = line.find("sonarefx ");
                if (p == std::string::npos) continue;
                const std::string s = line.substr(p + 9);
                if (s.compare(0, 8, "hook=ok ") == 0) {
                    efxStatus = EfxHumanStatus(s);
                } else if (s.compare(0, 7, "status:") == 0) {
                    efxStatus = s.substr(8);
                } else if (s.compare(0, 6, "index:") == 0) {
                    efxIndexInfo = s.substr(7);
                }
                continue;
            }
            if (line.compare(0, 7, "efxraw ") == 0) {     // 诊断：不在索引里的原始调用（取样给界面）
                if (efxRawSample.size() < 200) {
                    if (!efxRawSample.empty()) efxRawSample += " · ";
                    efxRawSample += line.substr(7);
                }
                ++efxRawCount;
                continue;
            }
            if (line.compare(0, 4, "efx ") != 0) continue;
            char table[64] = {}, cand[128] = {}, name[512] = {};
            unsigned seq = 0, via = 0, group = 0, rec = 0;
            int recId = -1, weapon = -1;
            // ★ sscanf_s 必须在**指针后面**额外传缓冲区大小（写进格式串会让它从栈上
            //   读到垃圾长度 → 越界 → 0xC0000005 崩溃，实测就是这么崩的）。
            if (sscanf_s(line.c_str(),
                         "efx seq#%u via=T%u table=%63s cand=%127s group=%u rec=%u id=%d weapon=%d name=%511[^\n]",
                         &seq, &via, table, (unsigned)sizeof(table), cand, (unsigned)sizeof(cand), &group, &rec, &recId, &weapon,
                         name, (unsigned)sizeof(name)) < 9) continue;
            ++efxOkCount;
            const bool sameTop = (!efxHist.empty() &&
                                  efxHist.front().efxTable == table &&
                                  efxHist.front().efxGroup == (int)group &&
                                  efxHist.front().efxRec == (int)rec);
            bool dup = sameTop;
            if (!dup) {
                for (std::size_t i = 0; i < efxHist.size(); ++i) {
                    const HistEntry& ph = efxHist[i];
                    if (nowMs - ph.ms >= 500) break;
                    if (ph.efxTable == table && ph.efxGroup == (int)group && ph.efxRec == (int)rec) { dup = true; break; }
                }
            }
            if (dup) { ++efxDup; continue; }
            HistEntry h;
            h.kind = 2;
            h.efxTable = table;
            h.efxGroup = (int)group;
            h.efxRec = (int)rec;
            h.efxRecId = recId;
            h.efxVia = (int)via;
            h.efxCand = cand;
            h.efxPath = name;
            h.weapon = weapon;
            h.ms = nowMs;
            h.time = TimeNowHms();
            efxHist.insert(efxHist.begin(), h);
            if (efxHist.size() > 300) efxHist.pop_back();
            ++efxRev;
            curEfxTable = table;
            curEfxGroup = (int)group;
            curEfxRec = (int)rec;
            curEfxVia = (int)via;
            curEfxPath = name;
            curEfxWeapon = weapon;
        }
        efxLogPos = (unsigned long long)efxStream.tellg();
        if (efxLogPos == (unsigned long long)-1) efxLogPos = 0;   // tellg 失败
        efxStream.clear();
        efxStream.seekg(0, std::ios::end);
    }
    JVal EfxJson() const {
        JVal a = JVal::arr();
        for (std::size_t i = 0; i < efxHist.size(); ++i) {
            const HistEntry& e = efxHist[i];
            JVal h = JVal::obj();
            h.set("kind", JVal(2));
            h.set("table", JVal(e.efxTable));
            h.set("group", JVal(e.efxGroup));
            h.set("rec", JVal(e.efxRec));
            h.set("recId", JVal(e.efxRecId));
            h.set("via", JVal(e.efxVia));
            h.set("cand", JVal(e.efxCand));
            h.set("name", JVal(e.efxPath));
            h.set("weapon", JVal(e.weapon));
            h.set("time", JVal(e.time));
            h.set("ageMs", JVal((long long)(::GetTickCount64() - e.ms)));
            a.push(h);
        }
        return a;
    }
    JVal WemJson() const {
        JVal a = JVal::arr();
        for (std::size_t i = 0; i < wemHist.size(); ++i) {
            const HistEntry& e = wemHist[i];
            JVal h = JVal::obj();
            h.set("kind", JVal(1));
            h.set("wemMedia", JVal(e.wemMedia));
            h.set("bank", JVal(e.bank));
            h.set("name", JVal(e.wemName));
            h.set("dispName", JVal(WemDispName((int)e.wemMedia, e.wemName, e.bank)));
            h.set("custom", JVal(wemUserNames.find(e.wemMedia) != wemUserNames.end()));
            {
                const std::unordered_map<int, std::string>::const_iterator nt = wemNotes.find(e.wemMedia);
                if (nt != wemNotes.end()) h.set("note", JVal(nt->second));
            }
            const std::unordered_map<int, std::pair<std::string, int>>::const_iterator sit =
                wemSeqs.find(e.wemMedia);
            if (sit != wemSeqs.end()) {
                h.set("seqNum", JVal(sit->second.second));
                if (e.bank.empty()) h.set("bank", JVal(sit->second.first));
            } else {
                std::string b; int n = 0;
                if (ParseSeqFromName(e.wemName, b, n)) {
                    h.set("seqNum", JVal(n));
                    if (e.bank.empty()) h.set("bank", JVal(b));
                }
            }
            h.set("weapon", JVal(e.weapon));
            h.set("weaponId", JVal(e.weaponId));
            h.set("time", JVal(e.time));
            h.set("ageMs", JVal((long long)(::GetTickCount64() - e.ms)));
            h.set("added", JVal(IsWemAdded(e.wemMedia)));
            a.push(h);
        }
        return a;
    }

    bool IsWemAdded(int wemMedia) const {
        if (wemMedia <= 0) return false;
        for (std::size_t i = 0; i < cfg.entries.size(); ++i) {
            const SoundEntry& e = cfg.entries[i];
            if (!EntryActive(e)) continue;
            if (e.media == (long long)wemMedia) return true;
        }
        return false;
    }

    bool IsCapturedAdded(int weapon, int fsm, int lmt) const {
        for (std::size_t i = 0; i < cfg.entries.size(); ++i) {
            const SoundEntry& e = cfg.entries[i];
            if (!EntryActive(e)) continue;
            if (e.fsmId != fsm) continue;
            if (e.weaponType >= 0 && e.weaponType != weapon) continue;
            if (!e.MatchesLmt(lmt)) continue;
            return true;
        }
        return false;
    }

    // 该动作（武器+FSM+LMT）当前命中几条条目 —— 捕获历史里用来区分"同一触发条件写了好几条"
    int CapturedMatchCount(int weapon, int fsm, int lmt) const {
        int n = 0;
        for (std::size_t i = 0; i < cfg.entries.size(); ++i) {
            const SoundEntry& e = cfg.entries[i];
            if (!EntryActive(e)) continue;
            if (e.fsmId != fsm) continue;
            if (e.weaponType >= 0 && e.weaponType != weapon) continue;
            if (!e.MatchesLmt(lmt)) continue;
            ++n;
        }
        return n;
    }

    JVal LiveJson() const {
        JVal d = JVal::obj();
        const bool att = live.attached && liveOk;
        d.set("attached", JVal(att));
        d.set("error", JVal(live.error.empty() ? game.LastError() : live.error));
        d.set("pid", JVal((long long)live.pid));
        d.set("inScene", JVal(live.inScene));
        d.set("fsm", JVal(live.inScene ? live.fsm : -1));
        d.set("lmt", JVal(live.inScene ? live.lmt : -1));
        d.set("weapon", JVal(live.inScene ? live.weapon : -1));
        d.set("weaponId", JVal(live.inScene ? live.weaponId : -1));
        d.set("weaponName", JVal(live.inScene ? std::string(WeaponName(live.weapon)) : std::string()));
        d.set("name", JVal(live.inScene
                               ? ResolveName(live.weapon, live.fsm, live.lmt)
                               : std::string()));
        d.set("added", JVal(live.inScene && IsCapturedAdded(live.weapon, live.fsm, live.lmt)));
        d.set("matches", JVal(live.inScene ? CapturedMatchCount(live.weapon, live.fsm, live.lmt) : 0));
        d.set("wemMedia", JVal(curWemMedia));      // 最近一次 wem 播放（无则 -1）
        d.set("wemName", JVal(curWemName.empty() ? std::string() : curWemName));
        d.set("wemDispName", JVal(curWemMedia > 0 ? WemDispName((int)curWemMedia, curWemName, curWemBank) : std::string()));
        d.set("wemBank", JVal(curWemBank.empty() ? std::string() : curWemBank));
        d.set("wemWeapon", JVal(curWemWeapon));
        d.set("der", DerJson());
        d.set("wem", WemJson());
        // 特效捕获（实验性）
        d.set("efx", EfxJson());
        d.set("efxEnabled", JVal(cfg.global.efxCapture != 0));
        d.set("efxRev", JVal(efxRev));
        d.set("efxCount", JVal((int)efxHist.size()));
        d.set("efxDup", JVal(efxDup));
        d.set("efxOk", JVal(efxOkCount));
        d.set("efxStatus", JVal(efxStatus));
        d.set("efxStats", JVal(efxStats));
        d.set("efxIndex", JVal(efxIndexInfo));
        d.set("efxRawSample", JVal(efxRawSample));
        d.set("efxRawCount", JVal(efxRawCount));
        // 诊断：特效日志路径是否解析到、流是否打开（"一条都没有"时先看这个）
        {
            std::string bn = efxLogPath;
            const std::size_t sl2 = bn.find_last_of("\\/");
            if (sl2 != std::string::npos) bn = bn.substr(sl2 + 1);
            d.set("efxLogName", JVal(bn));
            d.set("efxStreamOpen", JVal(efxStream.is_open()));
        }
        d.set("efxTable", JVal(curEfxTable));
        d.set("efxGroup", JVal(curEfxGroup));
        d.set("efxRec", JVal(curEfxRec));
        d.set("efxWeapon", JVal(curEfxWeapon));
        d.set("efxPath", JVal(curEfxPath));
        d.set("derRev", JVal(derRev));
        d.set("wemRev", JVal(wemRev));
        d.set("derCount", JVal((int)derHist.size()));
        d.set("wemCount", JVal((int)wemHist.size()));
        d.set("derDup", JVal(derDup));
        d.set("wemDup", JVal(wemDup));
        d.set("wemLoopDrop", JVal(wemLoopDrop));
        d.set("wemOk", JVal(wemOkCount));
        d.set("wemZero", JVal(wemZeroCount));
        d.set("wemNameCustom", JVal(curWemMedia > 0 &&
                                    wemUserNames.find(curWemMedia) != wemUserNames.end()));
        {
            const std::unordered_map<int, std::string>::const_iterator nt = wemNotes.find(curWemMedia);
            if (nt != wemNotes.end()) d.set("wemNote", JVal(nt->second));
        }

        // 当前场景（DLL 写 plugins\SonarScene.txt：mapId\nquestState；GUI 1s 缓存读取）
        {
            static unsigned long long s_scenePoll = 0;
            static std::string s_mapFileDir;   // plugins 根目录（wemLogPath 的父目录）
            unsigned long long nowMs2 = ::GetTickCount64();
            if (nowMs2 - s_scenePoll >= 1000 || !s_scenePoll) {
                s_scenePoll = nowMs2;
                if (s_mapFileDir.empty() && !wemLogPath.empty()) {
                    const std::size_t sp = wemLogPath.find_last_of("\\/");
                    if (sp != std::string::npos) s_mapFileDir = wemLogPath.substr(0, sp + 1);
                }
                if (!s_mapFileDir.empty()) {
                    std::string t;
                    if (FsRead(s_mapFileDir + "SonarScene.txt", t)) {
                        // 行：mapId / questState / 任务累计伤害 / 怪物血量 / 血量上限
                        //（后三行是新增的；老 DLL 只写两行 → 解析不到就当"读不到"）
                        std::vector<std::string> ln;
                        {
                            std::size_t p0 = 0;
                            while (p0 <= t.size()) {
                                std::size_t e = t.find('\n', p0);
                                if (e == std::string::npos) e = t.size();
                                ln.push_back(t.substr(p0, e - p0));
                                if (e == t.size()) break;
                                p0 = e + 1;
                            }
                        }
                        g_sceneMapId      = ln.size() > 0 ? std::atoi(ln[0].c_str()) : -1;
                        g_sceneQuestState = ln.size() > 1 ? std::atoi(ln[1].c_str()) : -1;
                        g_sceneQuestDmg   = ln.size() > 2 ? std::atoi(ln[2].c_str()) : -1;
                        g_sceneMonHp      = ln.size() > 3 ? (float)std::atof(ln[3].c_str()) : -1.0f;
                        g_sceneMonHpMax   = ln.size() > 4 ? (float)std::atof(ln[4].c_str()) : -1.0f;
                    } else {
                        g_sceneMapId = -1; g_sceneQuestState = -1;
                        g_sceneQuestDmg = -1; g_sceneMonHp = -1.0f; g_sceneMonHpMax = -1.0f;
                    }
                }
            }
            d.set("mapId", JVal(g_sceneMapId));
            d.set("questState", JVal(g_sceneQuestState));
            // 判定信号可用性：伤害类条件（dmg>0）需要"任务累计伤害"或"怪物血量"至少一路可读
            {
                const bool questOk = (g_sceneQuestDmg >= 0);
                const bool hpOk = (g_sceneMonHp >= 0.0f && g_sceneMonHpMax >= 10.0f);
                d.set("questDmg", JVal(g_sceneQuestDmg));
                d.set("monHp", JVal((double)g_sceneMonHp));
                d.set("monHpMax", JVal((double)g_sceneMonHpMax));
                d.set("dmgSignal", JVal(std::string(questOk ? "quest" : (hpOk ? "hp" : "none"))));
            }
        }
        {
            const std::unordered_map<int, std::pair<std::string, int>>::const_iterator sit =
                wemSeqs.find(curWemMedia);
            if (sit != wemSeqs.end()) {
                d.set("wemSeqNum", JVal(sit->second.second));
                if (curWemBank.empty()) d.set("wemBank", JVal(sit->second.first));
            } else {
                std::string b; int n = 0;
                if (ParseSeqFromName(curWemName, b, n)) {
                    d.set("wemSeqNum", JVal(n));
                    if (curWemBank.empty()) d.set("wemBank", JVal(b));
                }
            }
        }
        d.set("wemAdded", JVal(curWemMedia > 0 && IsWemAdded(curWemMedia)));
        return d;
    }

    // ---- 其它小工具 ----
    void SetStatus(const std::string& s) { status = s; }

    JVal WeaponListJson() const {
        JVal arr = JVal::arr();
        for (int w = 0; w <= 13; ++w) {
            JVal o = JVal::obj();
            o.set("id", JVal(w));
            o.set("name", JVal(std::string(WeaponName(w))));
            o.set("icon", JVal("w" + std::to_string(w)));
            o.set("count", JVal(CountFor(w)));
            const std::string ac = ActiveCombo(w);
            o.set("activeCombo", JVal(ac));
            o.set("hasNamed", JVal(WeaponCombos(w).size() > 1));
            arr.push(o);
        }
        // 任意武器：不存在"组合"概念，count 只数默认组合
        {
            JVal o = JVal::obj();
            o.set("id", JVal(-1));
            o.set("name", JVal(std::string(WeaponName(-1))));
            o.set("icon", JVal(std::string("wany")));
            o.set("count", JVal(CountFor(-1)));
            o.set("activeCombo", JVal(std::string()));
            o.set("hasNamed", JVal(false));
            arr.push(o);
        }
        return arr;
    }

    JVal GlobalJson() const {
        const GlobalSettings& g = cfg.global;
        JVal v = JVal::obj();
        v.set("playerRoot", JVal(g.playerRoot));
        v.set("pollMs", JVal(g.pollMs));
        v.set("debounceMs", JVal(g.debounceMs));
        v.set("volume", JVal(g.volume));
        v.set("enabled", JVal(g.enabled));
        v.set("moreSounds", JVal(g.moreSounds));
        v.set("gaugePtrOff", JVal(g.gaugePtrOff));
        v.set("gaugeValOff", JVal(g.gaugeValOff));
        v.set("chargeValOff", JVal(g.chargeValOff));
        v.set("fsmTargetOff", JVal(g.fsmTargetOff));
        v.set("questRoot", JVal(g.questRoot));
        v.set("questDmgOff", JVal(g.questDmgOff));
        v.set("debug", JVal(g.debug));
        v.set("chatEcho", JVal(g.chatEcho));
        v.set("updateCheck", JVal(g.updateCheck));
        v.set("updateProxy", JVal(g.updateProxy));
        v.set("chatCommands", JVal(g.chatCommands));
        v.set("hotkeysEnabled", JVal(g.hotkeysEnabled));
        return v;
    }

    JVal HotkeysJson() const {
        const Hotkeys& h = cfg.hotkeys;
        JVal v = JVal::obj();
        v.set("modifierKey", JVal(h.modifierKey));
        v.set("reloadKey", JVal(h.reloadKey));
        v.set("volUpKey", JVal(h.volUpKey));
        v.set("volDownKey", JVal(h.volDownKey));
        v.set("setVolKey", JVal(h.setVolKey));
        v.set("setVolValue", JVal(h.setVolValue));
        v.set("toggleKey", JVal(h.toggleKey));
        v.set("moreKey", JVal(h.moreKey));
        v.set("comboKey", JVal(h.comboKey));
        return v;
    }

    JVal ActiveJson() const {
        JVal v = JVal::obj();
        for (std::map<int, std::string>::const_iterator it = cfg.active.begin();
             it != cfg.active.end(); ++it) {
            if (it->second.empty()) continue;
            v.set(std::to_string(it->first), JVal(it->second));
        }
        return v;
    }

    // 明细列表（前端自己排序/筛选：中文拼音排序交给 Intl.Collator）
    JVal EntriesJson() const {
        JVal arr = JVal::arr();
        for (std::size_t i = 0; i < cfg.entries.size(); ++i)
            arr.push(EntryToJson(cfg.entries[i], (int)i));
        return arr;
    }

    // ---- 更新：状态快照（前端 poll 或事件都能拿到）----
    JVal UpdateData() const {
        JVal d = JVal::obj();
        d.set("state", JVal(updState.load()));
        d.set("progress", JVal(updProgress.load()));
        d.set("busy", JVal(updState.load() == 1));
        d.set("installing", JVal(updInstalling.load()));
        d.set("needRestart", JVal(updNeedRestart.load()));
        d.set("sameVer", JVal(updSameVer.load()));   // 版本号相同但仓库资产更新（热修复包）
        d.set("version", JVal(std::string(kWseGuiVersion)));
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(updMx));
        d.set("tag", JVal(updTag));
        d.set("url", JVal(updUrl));
        d.set("notes", JVal(updNotes));
        d.set("err", JVal(updErr));
        d.set("msg", JVal(updMsg));
        d.set("releaseUrl", JVal(std::string(kReleasesPage)));
        return d;
    }
};

// ===========================================================================
//  四、Core 门面
// ===========================================================================
Core::Core() : mImpl(new Impl()) {}
Core::~Core() { delete mImpl; }

namespace {
// Core 的 Impl 是私有嵌套类型（头文件里定的，不改动），但本文件里的静态辅助函数
// Core::Impl 与 Core::mImpl 已在 core.h 里公开（属于内部实现细节），这里直接取。
// 【踩过的坑】下面这段原来是 reinterpret_cast<CoreView*> 去偷 mImpl 的：
// CoreView 里加了个虚析构想让 vptr"占住偏移 0 对齐 Core 的布局"，但 Core 里
// mImpl 本来就在偏移 0、mOwner 在 8；加了 vptr 之后 mImpl 被挤到 16，
// 读出来是越界垃圾 —— 一跑就崩。别做布局投机。
Core::Impl* ImplOf(void* corePtr) {
    return corePtr ? static_cast<Core*>(corePtr)->mImpl : nullptr;
}
} // namespace

// 进度回调要跨 TU 用，写成文件内静态原子量（旧版也是全局原子量）
namespace {
std::atomic<int> gUpdProgress{-1};
void UpdProgressCb(std::size_t written, std::size_t total) {
    if (total > 0) gUpdProgress.store((int)(written * 100 / total));
    else gUpdProgress.store(-1);
}
} // namespace

// ---------------------------------------------------------------------------
//  文件对话框：全部以 mOwner 为所有者窗口
//
//  为什么一定要 owner：没有 owner 的对话框不属于任何窗口，会跑到任务栏上、
//  并且主窗口还能继续点（非模态观感），关掉后 z-order 也乱掉；WebView2 宿主
//  传进来的 HWND 就是主窗口，给它即可。
//  为什么不用 IFileOpenDialog：COM 那套要 CoInitialize + 版本判断，
//  GetOpenFileNameW 已经够用，而且旧版就是这么写的（行为一致、坑都踩过了）。
// ---------------------------------------------------------------------------
namespace {

HWND OwnerOf(void* owner) {
    HWND h = (HWND)owner;
    return (h && ::IsWindow(h)) ? h : nullptr;
}

// 选文件夹（音效替换的导出目录用）。SHBrowseForFolder 不需要 COM 初始化。
// 初始目录要用回调设置（BROWSEINFO 本身没有 lpszInitialPath 字段）。
static int CALLBACK BrowseInitProc(HWND hwnd, UINT msg, LPARAM, LPARAM data) {
    if (msg == BFFM_INITIALIZED && data)
        ::SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}
std::string PickFolder(void* owner, const std::wstring& initDir) {
    wchar_t disp[MAX_PATH * 2] = {};
    BROWSEINFOW bi{};
    bi.hwndOwner = OwnerOf(owner);
    bi.pszDisplayName = disp;
    bi.lpszTitle = L"选择导出目录（nbnk 会写到这里）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = BrowseInitProc;
    bi.lParam = initDir.empty() ? 0 : (LPARAM)initDir.c_str();
    LPITEMIDLIST idl = ::SHBrowseForFolderW(&bi);
    if (!idl) return std::string();
    std::wstring out;
    out.resize(MAX_PATH * 2);
    const BOOL ok = ::SHGetPathFromIDListW(idl, &out[0]);
    ::CoTaskMemFree(idl);
    if (!ok) return std::string();
    const std::size_t z = out.find(L'\0');
    if (z != std::wstring::npos) out.resize(z);
    return Utf8FromWide(out);
}

std::string OpenFileDialog(void* owner, const wchar_t* filter, const wchar_t* initDir,
                           const wchar_t* defExt, bool multi, std::vector<std::string>& multiOut) {
    multiOut.clear();
    // 多选缓冲要够大：OFN_ALLOWMULTISELECT 下 lpstrFile 装的是"目录\0文件1\0文件2..."，
    // 16384 个 wchar 能装几百个音效文件，旧版同样是这个数。
    std::vector<wchar_t> buf(multi ? 16384 : (MAX_PATH * 2), L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = OwnerOf(owner);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrInitialDir = initDir;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                (multi ? OFN_ALLOWMULTISELECT : 0);
    if (!::GetOpenFileNameW(&ofn)) return std::string();

    if (!multi) return Utf8FromWide(buf.data());

    // 多选：第一段是目录，后面每段一个文件名；只有一段时说明只选了一个文件
    std::vector<std::wstring> files;
    const wchar_t* p = buf.data();
    const std::wstring first = p;
    if (p[first.size() + 1] == L'\0') {
        files.push_back(first);
    } else {
        const std::wstring dir = first;
        p += first.size() + 1;
        while (*p) {
            const std::wstring fn = p;
            files.push_back(dir + L"\\" + fn);
            p += fn.size() + 1;
        }
    }
    for (std::size_t i = 0; i < files.size(); ++i) multiOut.push_back(Utf8FromWide(files[i]));
    return multiOut.empty() ? std::string() : multiOut[0];
}

std::string SaveFileDialog(void* owner, const wchar_t* filter, const wchar_t* defExt,
                           const std::wstring& initName) {
    std::vector<wchar_t> buf(MAX_PATH * 2, L'\0');
    if (!initName.empty()) {
        const std::size_t n = (initName.size() < buf.size() - 1) ? initName.size() : buf.size() - 1;
        std::memcpy(buf.data(), initName.c_str(), n * sizeof(wchar_t));
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = OwnerOf(owner);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!::GetSaveFileNameW(&ofn)) return std::string();
    return Utf8FromWide(buf.data());
}

} // namespace

// ---------------------------------------------------------------------------
//  把选中的音效文件换算成配置里要存的路径（浏览 / 替换共用）：
//   - 在数据目录 sounds\（或旧位置 exe 同目录 sounds\）里 → 存相对路径 sounds/…，保留层级；
//   - 否则存绝对路径（插件/试听直接读原文件）；
//   - 文件名含 ';' 或 ','（会破坏 ini 的列表分隔）→ 复制进 sounds\ 并改用相对路径。
// ---------------------------------------------------------------------------
namespace {

std::string RelPathForPicked(void* corePtr, const std::wstring& f, bool& copiedFallback) {
    Core::Impl* im = ImplOf(corePtr);
    const std::size_t slash = f.find_last_of(L"\\/");
    const std::wstring fn = (slash == std::wstring::npos) ? f : f.substr(slash + 1);
    const std::wstring fileDir = (slash == std::wstring::npos) ? L"" : f.substr(0, slash + 1);

    std::wstring targetNorm = Utf8ToWide(im->BaseDir() + "sounds");
    if (!targetNorm.empty() && targetNorm[targetNorm.size() - 1] != L'\\') targetNorm += L'\\';
    std::wstring legacyNorm = Utf8ToWide(im->exeDir + "sounds");
    if (!legacyNorm.empty() && legacyNorm[legacyNorm.size() - 1] != L'\\') legacyNorm += L'\\';

    const bool inTarget = !targetNorm.empty() && fileDir.size() >= targetNorm.size() &&
                          _wcsnicmp(fileDir.c_str(), targetNorm.c_str(), targetNorm.size()) == 0;
    const bool inLegacy = !legacyNorm.empty() && fileDir.size() >= legacyNorm.size() &&
                          _wcsnicmp(fileDir.c_str(), legacyNorm.c_str(), legacyNorm.size()) == 0;

    // 含分隔符的文件名会让 ini 解析出一堆不存在的音效，只能复制一份改名存放
    const bool bad = (Utf8FromWide(fn).find(';') != std::string::npos) ||
                     (Utf8FromWide(fn).find(',') != std::string::npos);

    std::string rel;
    if (inTarget || inLegacy) {
        const std::wstring base = inTarget ? targetNorm : legacyNorm;
        std::wstring sub = fileDir.substr(base.size());          // "" 或 "文件夹1\"
        for (std::size_t i = 0; i < sub.size(); ++i) if (sub[i] == L'\\') sub[i] = L'/';
        rel = "sounds/" + Utf8FromWide(sub) + Utf8FromWide(fn);
    } else {
        rel = ToSlash(Utf8FromWide(f));
    }
    if (bad) {
        ::CreateDirectoryW(targetNorm.c_str(), nullptr);
        CopyFileAtomic(f, targetNorm + fn);
        copiedFallback = true;
        rel = "sounds/" + Utf8FromWide(fn);
    }
    return rel;
}

// 试听 / 播放要把配置里的路径还原成磁盘路径
std::wstring ResolveSoundPath(void* corePtr, const std::string& rel) {
    Core::Impl* im = ImplOf(corePtr);
    if (rel.empty()) return std::wstring();
    if (IsAbsPath(rel)) return LongPathW(ToBack(rel));
    return LongPathW(im->BaseDir() + ToBack(rel));
}

// 弹多选文件对话框，把选中的音效追加为默认属性的 SoundSpec
JVal BrowseSounds(void* corePtr, void* owner, int& added) {
    Core::Impl* im = ImplOf(corePtr);
    added = 0;
    std::string sdir = im->BaseDir() + "sounds";
    std::wstring target = Utf8ToWide(sdir);
    // 旧布局：wav 可能还在与 DLL/exe 同级的 sounds\ 里。数据目录下还没有 sounds\
    // 时直接把对话框开在旧目录，免得用户以为音效丢了（选中后会复制/改写到数据目录）。
    const std::wstring legacySounds = Utf8ToWide(im->exeDir + "sounds");
    std::wstring initDir = target;
    if (::GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES &&
        ::GetFileAttributesW(legacySounds.c_str()) != INVALID_FILE_ATTRIBUTES)
        initDir = legacySounds;
    ::CreateDirectoryW(target.c_str(), nullptr);

    std::vector<std::string> picked;
    OpenFileDialog(owner,
                   L"音效 (*.wav;*.mp3;*.ogg;*.flac)\0*.wav;*.mp3;*.ogg;*.flac\0所有文件 (*.*)\0*.*\0",
                   initDir.c_str(), nullptr, true, picked);

    JVal arr = JVal::arr();
    bool fallbackCopied = false;
    // 去重靠调用方（前端拿 path 自己比）；这里只做路径换算
    std::set<std::string> seen;
    for (std::size_t i = 0; i < picked.size(); ++i) {
        const std::string rel = RelPathForPicked(corePtr, Utf8ToWide(picked[i]), fallbackCopied);
        if (rel.empty() || seen.count(rel)) continue;
        seen.insert(rel);
        SoundSpec sp;
        sp.path = rel;
        arr.push(SpecToJson(sp));
        ++added;
    }
    if (fallbackCopied)
        im->SetStatus("个别文件名里含 ; 或 ,（会破坏 ini 解析），已复制进 sounds\\ 并用相对路径");
    return arr;
}

// 单选：返回要存的路径（取消返回空）
std::string PickSoundFile(void* corePtr, void* owner, bool& cancelled) {
    Core::Impl* im = ImplOf(corePtr);
    cancelled = false;
    std::wstring target = Utf8ToWide(im->BaseDir() + "sounds");
    const std::wstring legacySounds = Utf8ToWide(im->exeDir + "sounds");
    std::wstring initDir = target;
    if (::GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES &&
        ::GetFileAttributesW(legacySounds.c_str()) != INVALID_FILE_ATTRIBUTES)
        initDir = legacySounds;
    ::CreateDirectoryW(target.c_str(), nullptr);

    std::vector<std::string> one;
    const std::string p = OpenFileDialog(
        owner, L"音效 (*.wav;*.mp3;*.ogg;*.flac)\0*.wav;*.mp3;*.ogg;*.flac\0所有文件 (*.*)\0*.*\0",
        initDir.c_str(), nullptr, false, one);
    if (p.empty()) { cancelled = true; return std::string(); }
    bool fb = false;
    return RelPathForPicked(corePtr, Utf8ToWide(p), fb);
}

// 导出核心：收集组合条目 + 把用到的音效复制到 dir\sounds\ 并改写条目路径
bool PackComboToDir(void* corePtr, int w, const std::string& combo, const std::string& dir,
                    std::vector<SoundEntry>& outEntries, int& copied, int& missing) {
    Core::Impl* im = ImplOf(corePtr);
    // 路径是 UTF-8 字节串，不能逐字节转 wchar（中文子目录会乱码）：
    // 先拼成完整 UTF-8 路径，再整体 Utf8ToWide。
    const std::wstring outDir = Utf8ToWide(dir + "sounds\\");
    ::CreateDirectoryW(outDir.c_str(), nullptr);

    std::map<std::wstring, std::string> srcToRel;   // 源文件(全小写) -> 导出后的相对路径
    std::map<std::wstring, int> usedBase;
    copied = 0;
    missing = 0;
    outEntries.clear();

    for (std::size_t i = 0; i < im->cfg.entries.size(); ++i) {
        const SoundEntry& e = im->cfg.entries[i];
        if (e.weaponType != w || e.combo != combo) continue;
        SoundEntry c2 = e;                              // 拷贝（含全部音效池）
        bool ok = true;
        // 四个池 + 条件池统一改写
        std::vector<std::vector<SoundSpec>*> pools;
        pools.push_back(&c2.def.specs);
        for (int g = 0; g < 4; ++g) pools.push_back(&c2.gauge[g].specs);
        for (std::size_t ci = 0; ci < c2.conds.size(); ++ci) pools.push_back(&c2.conds[ci].pool.specs);

        for (std::size_t pi = 0; pi < pools.size(); ++pi) {
            std::vector<SoundSpec>& specs = *pools[pi];
            for (std::size_t si = 0; si < specs.size(); ++si) {
                SoundSpec& sp = specs[si];
                // 源文件：sounds/相对 → 数据目录；找不到再试旧布局（exe 同级）
                std::wstring src;
                if (!IsAbsPath(sp.path) && !sp.path.empty()) {
                    const std::string cand = im->BaseDir() + ToBack(sp.path);
                    if (FsExists(cand)) src = Utf8ToWide(cand);
                    else {
                        const std::string cand2 = im->exeDir + ToBack(sp.path);
                        src = Utf8ToWide(cand2);
                    }
                } else if (IsAbsPath(sp.path)) {
                    src = Utf8ToWide(ToBack(sp.path));
                }
                if (src.empty() || ::GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    ++missing;                           // 找不到源文件：路径原样保留
                    continue;
                }
                std::wstring key = src;
                for (std::size_t k = 0; k < key.size(); ++k)
                    if (key[k] >= L'A' && key[k] <= L'Z') key[k] = (wchar_t)(key[k] + 32);
                std::map<std::wstring, std::string>::iterator it = srcToRel.find(key);
                if (it != srcToRel.end()) { sp.path = it->second; continue; }

                std::wstring outSub;   // outDir 之下的相对路径（含文件名）
                std::string newRel;
                if (sp.path.rfind("sounds/", 0) == 0 || sp.path.rfind("sounds\\", 0) == 0) {
                    // 配置里本来就是 sounds/…（可能带子文件夹）→ 保留子目录结构
                    std::string sub = sp.path.substr(7);
                    outSub = Utf8ToWide(sub);
                    for (std::size_t k = 0; k < outSub.size(); ++k)
                        if (outSub[k] == L'/') outSub[k] = L'\\';
                    const std::size_t lastBack = outSub.find_last_of(L'\\');
                    const std::wstring base =
                        outSub.substr(lastBack == std::wstring::npos ? 0 : lastBack + 1);
                    std::wstring baseKey = base;
                    for (std::size_t k = 0; k < baseKey.size(); ++k)
                        if (baseKey[k] >= L'A' && baseKey[k] <= L'Z') baseKey[k] = (wchar_t)(baseKey[k] + 32);
                    const int k2 = usedBase[baseKey];
                    if (k2 > 0) {                                  // 撞名：xxx_2.wav
                        const std::size_t dot = base.find_last_of(L'.');
                        const std::wstring n2 =
                            (dot == std::wstring::npos)
                                ? (base + L"_" + std::to_wstring(k2 + 1))
                                : (base.substr(0, dot) + L"_" + std::to_wstring(k2 + 1) + base.substr(dot));
                        outSub = (lastBack == std::wstring::npos)
                                     ? n2
                                     : (outSub.substr(0, lastBack + 1) + n2);
                    }
                    usedBase[baseKey] = k2 + 1;
                    newRel = "sounds/" + Utf8FromWide(outSub);
                    for (std::size_t k = 0; k < newRel.size(); ++k)
                        if (newRel[k] == '\\') newRel[k] = '/';
                } else {
                    // 绝对路径 → 压平到 sounds\ 根（带冲突改名）
                    std::wstring base = src.substr(src.find_last_of(L"\\/") + 1);
                    std::wstring baseKey = base;
                    for (std::size_t k = 0; k < baseKey.size(); ++k)
                        if (baseKey[k] >= L'A' && baseKey[k] <= L'Z') baseKey[k] = (wchar_t)(baseKey[k] + 32);
                    const int k2 = usedBase[baseKey];
                    std::wstring outName = base;
                    if (k2 > 0) {
                        const std::size_t dot = outName.find_last_of(L'.');
                        outName = (dot == std::wstring::npos)
                                      ? (outName + L"_" + std::to_wstring(k2 + 1))
                                      : (outName.substr(0, dot) + L"_" + std::to_wstring(k2 + 1) +
                                         outName.substr(dot));
                    }
                    usedBase[baseKey] = k2 + 1;
                    outSub = outName;
                    newRel = "sounds/" + Utf8FromWide(outName);
                }

                const std::size_t dd = outSub.find_last_of(L'\\');
                if (dd != std::wstring::npos) MakeDirs(outDir + outSub.substr(0, dd + 1));
                if (CopyFileAtomic(src, outDir + outSub)) ++copied;
                else ++missing;
                srcToRel[key] = newRel;
                sp.path = newRel;
            }
        }
        if (ok) outEntries.push_back(std::move(c2));
    }
    return !outEntries.empty();
}

// 合并一个 ini 到当前配置。importCombo=true（组合导入 / zip 包导入）时，
// 被导入文件的"默认组合"条目会放进自动新建的「导入」组合，绝不覆盖本地默认组合。
JVal MergeConfigFile(void* corePtr, const std::string& p, bool importCombo) {
    Core::Impl* im = ImplOf(corePtr);
    JVal res = JVal::obj();
    Config old;
    if (!LoadConfig(p, old)) {
        res.set("added", JVal(0));
        res.set("skipped", JVal(0));
        res.set("ok", JVal(false));
        res.set("error", JVal(std::string("读取失败: ") + p));
        im->SetStatus("读取失败: " + p);
        return res;
    }

    // 导入时的音效处理：按【组合名】分文件夹放进 sounds 下的组合名子目录
    int sndCopied = 0, sndMissing = 0;
    const std::string pkgSounds = [&] {
        const std::size_t s = p.find_last_of("\\/");
        return ((s == std::string::npos) ? std::string() : p.substr(0, s + 1)) + "sounds\\";
    }();
    const bool hasPkgSounds =
        ::GetFileAttributesW(Utf8ToWide(pkgSounds).c_str()) != INVALID_FILE_ATTRIBUTES;

    // 在包内 sounds\ 下按文件名递归查找（找不到再退回本地数据目录里的同名相对路径）
    struct Finder {
        static bool Find(const std::wstring& dir, const std::wstring& want, std::wstring& out) {
            WIN32_FIND_DATAW fd;
            HANDLE ff = ::FindFirstFileW((dir + L"*").c_str(), &fd);
            if (ff == INVALID_HANDLE_VALUE) return false;
            bool found = false;
            do {
                const std::wstring nm = fd.cFileName;
                if (nm == L"." || nm == L"..") continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (Find(dir + nm + L"\\", want, out)) { found = true; break; }
                } else if (_wcsicmp(nm.c_str(), want.c_str()) == 0) {
                    out = dir + nm;
                    found = true;
                    break;
                }
            } while (::FindNextFileW(ff, &fd));
            ::FindClose(ff);
            return found;
        }
    };

    // importCombo 时把"默认组合"条目改派到自动新建的「导入」组合（按武器各建一个）
    int relocated = 0;
    std::set<std::string> newCombos;
    std::map<int, std::string> newComboFirst;   // 武器 -> 第一个新建的组合（导入后自动切过去）
    std::map<int, std::string> assignedReloc;
    // 同一个武器的默认条目只分配一次【同一个】组合名（导入 / 导入2 / ...），
    // 绝不按条数拆成 导入、导入2、导入3……
    for (std::size_t i = 0; i < old.entries.size(); ++i) {
        SoundEntry& e = old.entries[i];
        if (!importCombo || e.weaponType < 0 || !e.combo.empty()) {
            if (importCombo && e.weaponType >= 0 && !e.combo.empty()) {
                bool exists = false;
                for (std::size_t k = 0; k < im->cfg.entries.size(); ++k)
                    if (im->cfg.entries[k].weaponType == e.weaponType &&
                        im->cfg.entries[k].combo == e.combo) { exists = true; break; }
                if (!exists && !newComboFirst.count(e.weaponType))
                    newComboFirst[e.weaponType] = e.combo;
            }
            continue;
        }
        std::map<int, std::string>::iterator it = assignedReloc.find(e.weaponType);
        if (it != assignedReloc.end()) { e.combo = it->second; ++relocated; continue; }
        std::string nm = "导入";
        for (int k2 = 2;; ++k2) {
            bool clash = false;
            for (std::size_t k = 0; k < im->cfg.entries.size(); ++k)
                if (im->cfg.entries[k].weaponType == e.weaponType &&
                    im->cfg.entries[k].combo == nm) { clash = true; break; }
            if (!clash) break;
            nm = "导入" + std::to_string(k2);
        }
        assignedReloc[e.weaponType] = nm;
        newCombos.insert(nm);
        if (!newComboFirst.count(e.weaponType)) newComboFirst[e.weaponType] = nm;
        e.combo = nm;
        ++relocated;
    }

    std::vector<std::string> have;
    have.reserve(im->cfg.entries.size() + old.entries.size());
    for (std::size_t i = 0; i < im->cfg.entries.size(); ++i)
        have.push_back(EntryKey(im->cfg.entries[i]));

    int added = 0, skipped = 0;
    for (std::size_t i = 0; i < old.entries.size(); ++i) {
        SoundEntry e = old.entries[i];

        // 导入：把这条条目用到的音效按【组合名】归到 sounds\<组合名>\ 下并改写路径
        if (importCombo && e.weaponType >= 0) {
            const std::string folder = ComboFolder(e.combo);
            const std::wstring dstDirW = Utf8ToWide(im->BaseDir() + "sounds\\" + folder + "\\");
            std::vector<std::vector<SoundSpec>*> pools;
            pools.push_back(&e.def.specs);
            for (int g = 0; g < 4; ++g) pools.push_back(&e.gauge[g].specs);
            for (std::size_t ci = 0; ci < e.conds.size(); ++ci) pools.push_back(&e.conds[ci].pool.specs);
            for (std::size_t pi = 0; pi < pools.size(); ++pi) {
                std::vector<SoundSpec>& specs = *pools[pi];
                for (std::size_t si = 0; si < specs.size(); ++si) {
                    SoundSpec& sp = specs[si];
                    if (sp.path.empty()) continue;
                    if (IsAbsPath(sp.path)) continue;                       // 绝对路径不动
                    if (sp.path.rfind("sounds/", 0) != 0 && sp.path.rfind("sounds\\", 0) != 0)
                        continue;
                    std::string base = sp.path;
                    const std::size_t sl = base.find_last_of("/\\");
                    if (sl != std::string::npos) base = base.substr(sl + 1);
                    if (base.empty()) continue;
                    const std::wstring baseW = Utf8ToWide(base);
                    // 源文件：先在包内 sounds\ 里按文件名找，再退回本机数据目录里的原路径
                    std::wstring src;
                    if (hasPkgSounds && !Finder::Find(Utf8ToWide(pkgSounds), baseW, src)) src.clear();
                    if (src.empty()) {
                        const std::string local = im->BaseDir() + ToBack(sp.path);
                        if (FsExists(local)) src = Utf8ToWide(local);
                    }
                    if (src.empty()) { ++sndMissing; continue; }        // 找不到：路径保持原样
                    MakeDirs(dstDirW);
                    const std::wstring dst = dstDirW + baseW;
                    if (::GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        if (CopyFileAtomic(src, dst)) ++sndCopied;
                        else { ++sndMissing; continue; }
                    }
                    sp.path = "sounds/" + folder + "/" + base;
                }
            }
        }

        const std::string k = EntryKey(e);
        bool dup = false;
        for (std::size_t h = 0; h < have.size(); ++h) if (have[h] == k) { dup = true; break; }
        if (dup) { ++skipped; continue; }
        im->cfg.entries.push_back(e);
        have.push_back(EntryKey(im->cfg.entries.back()));
        ++added;
    }
    if (added > 0) im->dirty = true;

    std::string extra;
    if (sndCopied > 0)
        extra = "；音效已按组合归到 sounds\\<组合名>\\（复制 " + std::to_string(sndCopied) + " 个）";
    if (sndMissing > 0)
        extra += "；有 " + std::to_string(sndMissing) + " 个音效在包里没找到，路径保持原样";
    im->SetStatus("导入配置: 新增 " + std::to_string(added) + " 条，跳过重复 " +
                  std::to_string(skipped) + " 条" + extra);

    // 导入出新组合时自动把对应武器的当前组合切过去：否则条目落在"非激活组合"里，
    // 列表和游戏里都看不到，用户会以为没导入成功。
    std::string switchedWeapon;
    if (importCombo && !newComboFirst.empty()) {
        std::string names;
        for (std::map<int, std::string>::iterator it = newComboFirst.begin();
             it != newComboFirst.end(); ++it) {
            if (im->ActiveCombo(it->first) == it->second) continue;
            im->cfg.active[it->first] = it->second;
            im->dirty = true;
            if (!names.empty()) names += "、";
            names += std::string(WeaponName(it->first)) + "→" + it->second;
            if (switchedWeapon.empty()) switchedWeapon = std::to_string(it->first);
        }
        if (!switchedWeapon.empty())
            im->status += "；已把「" + names + "」切为当前组合（左侧下拉可改回）";
    }

    std::string relocatedNames;
    for (std::set<std::string>::iterator it = newCombos.begin(); it != newCombos.end(); ++it) {
        if (!relocatedNames.empty()) relocatedNames += "、";
        relocatedNames += *it;
    }
    if (relocated > 0)
        im->status += "；原「默认组合」的 " + std::to_string(relocated) + " 条已导入为新组合「" +
                      relocatedNames + "」，没有覆盖你的默认组合";
    im->status += "（记得保存再重载游戏配置生效）";

    res.set("added", JVal(added));
    res.set("skipped", JVal(skipped));
    res.set("relocated", JVal(relocated));
    res.set("soundCopied", JVal(sndCopied));
    res.set("soundMissing", JVal(sndMissing));
    res.set("status", JVal(im->status));
    return res;
}

} // namespace

// ---------------------------------------------------------------------------
//  四、方法分发
// ---------------------------------------------------------------------------
std::string Core::Handle(const std::string& method, const std::string& paramsJson) {
    // 绝不让异常穿过边界：宿主是 C++/COM 回调栈，抛出去就是整个进程崩。
    try {
        if (!mImpl) return ErrJson("核心未初始化");
        Impl* im = mImpl;
        void* corePtr = (void*)this;   // 静态辅助函数用它反查 Impl
        const JVal p = ParseParams(paramsJson);
        void* owner = mOwner;

        // ---------------- 基础状态 ----------------
        if (method == "state") {
            JVal d = JVal::obj();
            d.set("version", JVal(std::string(kWseGuiVersion)));
            d.set("exeDir", JVal(ToSlash(im->exeDir)));
            d.set("dataDir", JVal(ToSlash(im->BaseDir())));
            d.set("iniPath", JVal(ToSlash(im->IniPath())));
            d.set("gameIniPath", JVal(ToSlash(im->gameIniPath)));
            d.set("isGameIni", JVal(im->IsGameIni()));
            d.set("loaded", JVal(im->cfg.loaded));
            d.set("dirty", JVal(im->dirty));
            d.set("uiPath", JVal(ToSlash(im->uiPath)));
            d.set("ui", im->UiPrefsJson());
            d.set("global", im->GlobalJson());
            d.set("hotkeys", im->HotkeysJson());
            d.set("active", im->ActiveJson());
            d.set("entryCount", JVal((int)im->cfg.entries.size()));
            d.set("weapons", im->WeaponListJson());
            d.set("theme", JVal(im->prefs.theme));
            d.set("defaultTheme", JVal(std::string(kDefaultTheme)));
            d.set("themes", ThemesJson());
            d.set("status", JVal(im->status));
            d.set("fsmDbCount", JVal((int)GetFsmDb().size()));
            return OkJson(d);
        }

        // ---------------- 配置读写 ----------------
        if (method == "config.load") {
            const std::string want = p.optStr("path");
            std::string path = want;
            if (path.empty()) path = im->IniPath();
            JVal warns = JVal::arr();
            if (!LoadConfig(path, im->cfg)) {
                const std::string msg = "加载失败: " + path;
                im->SetStatus(msg);
                return ErrJson(msg);
            }
            im->EnrichNames();
            SetFsmDbDir(im->BaseDir());
            ReloadFsmDb();
            im->dirty = false;
            im->SetStatus("已加载: " + path);
            if (im->cfg.fixedCombos > 0)
                im->status += "（已修正 " + std::to_string(im->cfg.fixedCombos) +
                              " 条错位条目：旧版保存会把默认组合的条目写进上一个组合的段里，"
                              "现在按各自武器的默认组合处理；保存一次就恢复正常）";
            if (im->cfg.fixedCombos > 0) warns.push(JVal(im->status));
            JVal d = JVal::obj();
            d.set("iniPath", JVal(ToSlash(im->IniPath())));
            d.set("warnings", warns);
            d.set("fixedCombos", JVal(im->cfg.fixedCombos));
            d.set("entryCount", JVal((int)im->cfg.entries.size()));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "config.save") {
            const std::string path = im->IniPath();
            if (!SaveConfig(path, im->cfg)) {
                im->SetStatus("保存失败: " + path);
                return ErrJson(im->status);
            }
            im->dirty = false;
            im->SetStatus("已保存: " + path);
            JVal d = JVal::obj();
            d.set("iniPath", JVal(ToSlash(path)));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "config.saveAs") {
            const std::string path = SaveFileDialog(
                owner, L"INI 配置文件 (*.ini)\0*.ini\0所有文件 (*.*)\0*.*\0", L"ini",
                L"WeaponSoundEnhance.ini");
            if (path.empty()) {
                JVal d = JVal::obj();
                d.set("cancelled", JVal(true));
                d.set("iniPath", JVal(ToSlash(im->IniPath())));
                return OkJson(d);
            }
            if (!SaveConfig(path, im->cfg)) {
                im->SetStatus("保存失败: " + path);
                return ErrJson(im->status);
            }
            im->cfg.path = path;
            im->cfg.loaded = true;
            im->dirty = false;
            SetFsmDbDir(im->BaseDir());   // 数据目录跟着 ini 走（fsm_db/sounds 都在那儿）
            ReloadFsmDb();
            im->SetStatus("已保存: " + path);
            JVal d = JVal::obj();
            d.set("iniPath", JVal(ToSlash(path)));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "config.open") {
            std::vector<std::string> one;
            const std::string path = OpenFileDialog(
                owner, L"INI 配置文件 (*.ini)\0*.ini\0所有文件 (*.*)\0*.*\0",
                nullptr, nullptr, false, one);
            if (path.empty()) {
                JVal d = JVal::obj();
                d.set("cancelled", JVal(true));
                d.set("iniPath", JVal(ToSlash(im->IniPath())));
                return OkJson(d);
            }
            JVal inner = JVal::obj();
            inner.set("path", JVal(path));
            return Handle("config.load", JsonDump(inner));
        }

        if (method == "config.switchToGameIni") {
            if (im->gameIniPath.empty()) {
                const std::string msg =
                    "找不到游戏目录（游戏没在运行？先用【另存为】存到 plugins\\WeaponSoundEnhance\\ 下）";
                im->SetStatus(msg);
                return ErrJson(msg);
            }
            if (FsExists(im->gameIniPath)) {
                JVal inner = JVal::obj();
                inner.set("path", JVal(im->gameIniPath));
                std::string r = Handle("config.load", JsonDump(inner));
                return r;
            }
            // 游戏那份还不存在：把当前配置写过去（原来的文件不动）
            const std::string old = im->cfg.path;
            if (!SaveConfig(im->gameIniPath, im->cfg)) {
                const std::string msg = "保存到游戏目录失败: " + im->gameIniPath;
                im->SetStatus(msg);
                return ErrJson(msg);
            }
            im->cfg.path = im->gameIniPath;
            im->cfg.loaded = true;
            im->dirty = false;
            SetFsmDbDir(im->BaseDir());
            ReloadFsmDb();
            im->SetStatus("已把当前配置保存到游戏目录: " + im->gameIniPath + "（原来的 " + old + " 没动）");
            JVal d = JVal::obj();
            d.set("iniPath", JVal(ToSlash(im->gameIniPath)));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "config.mergeOldIni") {
            std::vector<std::string> one;
            const std::string path = OpenFileDialog(
                owner, L"INI 配置文件 (*.ini)\0*.ini\0所有文件 (*.*)\0*.*\0",
                nullptr, nullptr, false, one);
            if (path.empty()) {
                JVal d = JVal::obj();
                d.set("added", JVal(0));
                d.set("cancelled", JVal(true));
                return OkJson(d);
            }
            JVal r = MergeConfigFile(corePtr, path, false);
            if (!r.has("added")) r.set("added", JVal(0));
            return OkJson(r);
        }

        // ---------------- 全局设置 ----------------
        if (method == "global.set") {
            const std::string key = Trim(p.optStr("key"));
            if (key.empty()) return ErrJson("缺少 key");
            GlobalSettings& g = im->cfg.global;
            const JVal* v = p.find("value");
            const std::string sv = v ? v->asStr() : std::string();
            const int iv = v ? v->asInt(0) : 0;
            // 每个字段都做范围检查：ini 里的数字插件会直接拿去当偏移轮询周期/音量用，
            // 写进一个 0 或负数会让插件疯狂空转甚至不响。
            if (key == "playerRoot") g.playerRoot = Trim(sv);
            else if (key == "pollMs") { g.pollMs = iv < 10 ? 10 : (iv > 2000 ? 2000 : iv); }
            else if (key == "debounceMs") { g.debounceMs = iv < 0 ? 0 : (iv > 5000 ? 5000 : iv); }
            else if (key == "volume") { g.volume = iv < 0 ? 0 : (iv > 100 ? 100 : iv); }
            else if (key == "enabled") g.enabled = v ? (v->asBool() ? 1 : 0) : 0;
            else if (key == "moreSounds") g.moreSounds = v ? (v->asBool() ? 1 : 0) : 0;
            else if (key == "gaugePtrOff") g.gaugePtrOff = Trim(sv);
            else if (key == "gaugeValOff") g.gaugeValOff = Trim(sv);
            else if (key == "chargeValOff") g.chargeValOff = Trim(sv);
            else if (key == "fsmTargetOff") g.fsmTargetOff = Trim(sv);
            else if (key == "questRoot") g.questRoot = Trim(sv);
            else if (key == "questDmgOff") g.questDmgOff = Trim(sv);
            else if (key == "debug") g.debug = v ? (v->asBool() ? 1 : 0) : 0;
            else if (key == "chatEcho") g.chatEcho = v ? (v->asBool() ? 1 : 0) : 0;
            else if (key == "updateCheck") g.updateCheck = v ? (v->asBool() ? 1 : 0) : 0;
            else if (key == "updateProxy") g.updateProxy = Trim(sv);
            else if (key == "chatCommands") g.chatCommands = v ? (v->asBool() ? 1 : 0) : 0;
            else if (key == "hotkeysEnabled") g.hotkeysEnabled = v ? (v->asBool() ? 1 : 0) : 0;
            else return ErrJson("未知设置项: " + key);
            im->dirty = true;
            JVal d = im->GlobalJson();
            return OkJson(d);
        }

        if (method == "hotkeys.set") {
            const std::string key = Trim(p.optStr("key"));
            if (key.empty()) return ErrJson("缺少 key");
            Hotkeys& h = im->cfg.hotkeys;
            const JVal* v = p.find("value");
            const int iv = v ? v->asInt(0) : 0;
            if (key == "modifierKey") h.modifierKey = iv;
            else if (key == "reloadKey") h.reloadKey = iv;
            else if (key == "volUpKey") h.volUpKey = iv;
            else if (key == "volDownKey") h.volDownKey = iv;
            else if (key == "setVolKey") h.setVolKey = iv;
            else if (key == "setVolValue") h.setVolValue = iv < 0 ? 0 : (iv > 100 ? 100 : iv);
            else if (key == "toggleKey") h.toggleKey = iv;
            else if (key == "moreKey") h.moreKey = iv;
            else if (key == "comboKey") h.comboKey = iv;
            else return ErrJson("未知热键项: " + key);
            im->dirty = true;
            return OkJson(im->HotkeysJson());
        }

        // ---------------- 条目 ----------------
        if (method == "entries.list") {
            const JVal* jw = p.find("weapon");
            const JVal* jc = p.find("combo");
            const std::string q = ToLowerAscii(Trim(p.optStr("q")));
            JVal arr = JVal::arr();
            // 排序/筛选交给前端（中文拼音排序用 Intl.Collator），这里只按 index 原序输出，
            // 顺带把 weapon/combo/q 三个可选筛选做掉，省得前端为了看一个组合拉全表。
            for (std::size_t i = 0; i < im->cfg.entries.size(); ++i) {
                const SoundEntry& e = im->cfg.entries[i];
                if (jw && jw->isNum()) {
                    const int w = jw->asInt(-99);
                    if (w != e.weaponType) continue;
                }
                if (jc && jc->isStr()) {
                    if (jc->s != e.combo) continue;      // 空串 = 默认组合（精确比较）
                }
                if (!q.empty()) {
                    bool hit = false;
                    const std::string nm = ToLowerAscii(e.name);
                    if (nm.find(q) != std::string::npos) hit = true;
                    if (!hit && e.fsmId >= 0 &&
                        std::to_string(e.fsmId).find(q) != std::string::npos) hit = true;
                    if (!hit) {
                        for (std::size_t k = 0; k < e.lmt.size() && !hit; ++k)
                            if (std::to_string(e.lmt[k]).find(q) != std::string::npos) hit = true;
                    }
                    if (!hit) {
                        for (std::size_t k = 0; k < e.def.specs.size() && !hit; ++k)
                            if (ToLowerAscii(e.def.specs[k].path).find(q) != std::string::npos) hit = true;
                    }
                    if (!hit && WeaponName(e.weaponType) &&
                        ToLowerAscii(WeaponName(e.weaponType)).find(q) != std::string::npos) hit = true;
                    if (!hit) continue;
                }
                JVal v = EntryToJson(e, (int)i);
                im->AttachSeq(v, e.media);   // wem 条目附 序号/归属 bank
                arr.push(v);
            }
            JVal d = JVal::obj();
            d.set("entries", arr);
            d.set("total", JVal((int)arr.a.size()));
            d.set("entryCount", JVal((int)im->cfg.entries.size()));
            d.set("weapons", im->WeaponListJson());
            d.set("active", im->ActiveJson());
            return OkJson(d);
        }

        if (method == "entries.get") {
            const int idx = p.optInt("index", -1);
            if (idx < 0 || idx >= (int)im->cfg.entries.size())
                return ErrJson("index 越界: " + std::to_string(idx));
            JVal d = JVal::obj();
            JVal ev = EntryToJson(im->cfg.entries[idx], idx);
            im->AttachSeq(ev, im->cfg.entries[idx].media);
            d.set("entry", ev);
            d.set("index", JVal(idx));
            return OkJson(d);
        }

        if (method == "entries.save") {
            const JVal* je = p.find("entry");
            if (!je || !je->isObj()) return ErrJson("缺少 entry 对象");
            const int idx = p.optInt("index", -1);
            SoundEntry e = EntryFromJson(*je, im->AllowedCombos(je->optInt("weaponType", -1)));
            if (idx < 0) {
                im->cfg.entries.push_back(e);
                im->dirty = true;
                im->EnrichNames();
                im->SetStatus("已新增条目（记得保存）");
                JVal d = JVal::obj();
                d.set("index", JVal((int)im->cfg.entries.size() - 1));
                d.set("entry", EntryToJson(im->cfg.entries.back(), (int)im->cfg.entries.size() - 1));
                d.set("status", JVal(im->status));
                return OkJson(d);
            }
            if (idx >= (int)im->cfg.entries.size())
                return ErrJson("index 越界: " + std::to_string(idx));
            // 编辑器没带 media 字段（老界面）→ 保留原值，别把 wem 条目的 id 抹掉
            if (!je->has("media")) e.media = im->cfg.entries[idx].media;
            if (!je->has("bank")) e.bank = im->cfg.entries[idx].bank;
            im->cfg.entries[idx] = e;
            im->dirty = true;
            im->SetStatus("已修改（记得保存）");
            JVal d = JVal::obj();
            d.set("index", JVal(idx));
            d.set("entry", EntryToJson(im->cfg.entries[idx], idx));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "entries.remove") {
            const JVal* ji = p.find("indices");
            if (!ji || !ji->isArr()) return ErrJson("缺少 indices 数组");
            std::vector<int> idx;
            for (std::size_t i = 0; i < ji->a.size(); ++i) {
                const int v = ji->a[i].asInt(-1);
                if (v >= 0 && v < (int)im->cfg.entries.size()) idx.push_back(v);
            }
            // 从后往前删：先删小的会把后面所有下标挤掉
            std::sort(idx.begin(), idx.end());
            idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
            int removed = 0;
            for (std::size_t i = idx.size(); i-- > 0;) {
                im->cfg.entries.erase(im->cfg.entries.begin() + idx[i]);
                ++removed;
            }
            if (removed > 0) {
                im->dirty = true;
                im->SetStatus("已删除 " + std::to_string(removed) + " 条条目（记得保存）");
            }
            JVal d = JVal::obj();
            d.set("removed", JVal(removed));
            d.set("entryCount", JVal((int)im->cfg.entries.size()));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "entries.duplicate") {
            const int idx = p.optInt("index", -1);
            if (idx < 0 || idx >= (int)im->cfg.entries.size())
                return ErrJson("index 越界: " + std::to_string(idx));
            SoundEntry copy = im->cfg.entries[idx];
            im->cfg.entries.insert(im->cfg.entries.begin() + idx + 1, copy);
            im->dirty = true;
            im->SetStatus("已复制条目（记得保存）");
            JVal d = JVal::obj();
            d.set("index", JVal(idx + 1));
            d.set("entry", EntryToJson(im->cfg.entries[idx + 1], idx + 1));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        // ---------------- 配置组合 ----------------
        if (method == "combo.list") {
            const int w = p.optInt("weapon", -1);
            if (w < -1 || w > 13) return ErrJson("weapon 越界");
            std::vector<std::string> combos = (w < 0) ? std::vector<std::string>(1, "")
                                                      : im->WeaponCombos(w);
            JVal arr = JVal::arr();
            for (std::size_t i = 0; i < combos.size(); ++i) arr.push(JVal(combos[i]));
            JVal d = JVal::obj();
            d.set("combos", arr);
            d.set("active", JVal(im->ActiveCombo(w)));
            d.set("weapon", JVal(w));
            // 每把武器的组合名（前端左栏要把每把武器的组合都列出来）
            JVal all = JVal::obj();
            for (int k = 0; k <= 13; ++k) {
                std::vector<std::string> cs = im->WeaponCombos(k);
                JVal a2 = JVal::arr();
                for (std::size_t i = 0; i < cs.size(); ++i) a2.push(JVal(cs[i]));
                all.set(std::to_string(k), a2);
            }
            d.set("all", all);
            return OkJson(d);
        }

        if (method == "combo.create") {
            const int w = p.optInt("weapon", -1);
            const std::string name = Trim(p.optStr("name"));
            if (w < 0 || w > 13) return ErrJson("weapon 越界");
            if (name.empty()) return ErrJson("组合名不能为空");
            std::vector<std::string> combos = im->WeaponCombos(w);
            for (std::size_t i = 0; i < combos.size(); ++i)
                if (combos[i] == name) return ErrJson("组合「" + name + "」已存在");
            // 新组合从空白开始：不复制当前组合的条目。
            // （用户明确要求：新组合里的条目音效应为空，而不是把旧音效复制过来）
            // ★ 必须把名字写进 comboList 持久化列表：它还没有条目，不登记的话
            //   "按条目推导组合"会把它当不存在 → 下拉里看不到 = "新增没反应"。
            {
                std::vector<std::string>& lst = im->cfg.comboList[w];
                bool inList = false;
                for (std::size_t i = 0; i < lst.size(); ++i) if (lst[i] == name) { inList = true; break; }
                if (!inList) lst.push_back(name);
            }
            im->cfg.active[w] = name;
            im->dirty = true;
            im->SetStatus("已新建组合「" + name + "」（空白组合，记得保存）");
            if (im->cfg.loaded && !im->cfg.path.empty())
                (void)SaveConfig(im->cfg.path, im->cfg);
            JVal d = JVal::obj();
            d.set("copied", JVal(0));
            d.set("active", JVal(name));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        // 把一个组合的条目整体复制到另一个组合（可跨武器；replace=true 先清空目标）
        if (method == "combo.copyEntries") {
            const int fw = p.optInt("fromWeapon", -1);
            const int tw = p.optInt("toWeapon", -1);
            const std::string fc = p.optStr("fromCombo");
            const std::string tc = p.optStr("toCombo");
            const bool replace = p.optBool("replace", false);
            if (fw < 0 || fw > 13 || tw < 0 || tw > 13) return ErrJson("武器越界");
            if (fw == tw && fc == tc) return ErrJson("源组合和目标组合是同一个");
            std::vector<SoundEntry> src;
            for (std::size_t i = 0; i < im->cfg.entries.size(); ++i)
                if (im->cfg.entries[i].weaponType == fw && im->cfg.entries[i].combo == fc)
                    src.push_back(im->cfg.entries[i]);
            if (src.empty()) return ErrJson("源组合里没有条目可复制");
            if (replace) {
                std::vector<SoundEntry> keep;
                for (std::size_t i = 0; i < im->cfg.entries.size(); ++i)
                    if (!(im->cfg.entries[i].weaponType == tw && im->cfg.entries[i].combo == tc))
                        keep.push_back(im->cfg.entries[i]);
                im->cfg.entries.swap(keep);
            }
            for (std::size_t i = 0; i < src.size(); ++i) {
                SoundEntry c = src[i];
                c.weaponType = tw;
                c.combo = tc;
                im->cfg.entries.push_back(c);
            }
            if (!tc.empty()) {   // 目标组合登记，避免"按条目推导"漏掉
                std::vector<std::string>& lst = im->cfg.comboList[tw];
                bool inList = false;
                for (std::size_t i = 0; i < lst.size(); ++i) if (lst[i] == tc) { inList = true; break; }
                if (!inList) lst.push_back(tc);
            }
            im->dirty = true;
            im->SetStatus("已复制 " + std::to_string(src.size()) + " 条条目到目标组合");
            if (im->cfg.loaded && !im->cfg.path.empty())
                (void)SaveConfig(im->cfg.path, im->cfg);
            JVal d = JVal::obj();
            d.set("copied", JVal((int)src.size()));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "combo.rename") {
            const int w = p.optInt("weapon", -1);
            const std::string from = p.optStr("from");
            const std::string to = Trim(p.optStr("to"));
            if (from.empty()) return ErrJson("默认组合不能重命名");
            if (to.empty()) return ErrJson("新组合名不能为空");
            if (to == from) return OkJson();
            std::vector<std::string> combos = im->WeaponCombos(w);
            for (std::size_t i = 0; i < combos.size(); ++i)
                if (combos[i] == to) return ErrJson("组合「" + to + "」已存在");
            int n = 0;
            for (std::size_t i = 0; i < im->cfg.entries.size(); ++i) {
                SoundEntry& e = im->cfg.entries[i];
                if (e.weaponType == w && e.combo == from) { e.combo = to; ++n; }
            }
            // 组合列表改名（老配置可能没登记 from，先补录再改名）
            {
                std::vector<std::string>& lst = im->cfg.comboList[w];
                bool fromIn = false;
                for (std::size_t i = 0; i < lst.size(); ++i) if (lst[i] == from) { lst[i] = to; fromIn = true; break; }
                if (!fromIn) {
                    bool toIn = false;
                    for (std::size_t i = 0; i < lst.size(); ++i) if (lst[i] == to) { toIn = true; break; }
                    if (!toIn) lst.push_back(to);
                }
            }
            if (im->ActiveCombo(w) == from) im->cfg.active[w] = to;
            im->dirty = true;
            im->SetStatus("组合「" + from + "」已重命名为「" + to + "」（" + std::to_string(n) +
                          " 条，记得保存）");
            if (im->cfg.loaded && !im->cfg.path.empty())
                (void)SaveConfig(im->cfg.path, im->cfg);
            JVal d = JVal::obj();
            d.set("moved", JVal(n));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "combo.remove") {
            const int w = p.optInt("weapon", -1);
            const std::string name = p.optStr("name");
            if (name.empty()) return ErrJson("默认组合不能整个删除（可以逐条删条目）");
            // 从持久化列表移除（空组合也要从列表删掉，否则删了还显示在列表里）
            {
                std::vector<std::string>& lst = im->cfg.comboList[w];
                for (std::size_t i = 0; i < lst.size(); ++i) {
                    if (lst[i] == name) { lst.erase(lst.begin() + i); break; }
                }
            }
            int n = 0;
            for (std::size_t i = 0; i < im->cfg.entries.size();) {
                if (im->cfg.entries[i].weaponType == w && im->cfg.entries[i].combo == name) {
                    im->cfg.entries.erase(im->cfg.entries.begin() + i);
                    ++n;
                } else {
                    ++i;
                }
            }
            if (im->ActiveCombo(w) == name) im->cfg.active[w] = "";
            im->dirty = true;
            im->SetStatus("已删除组合「" + name + "」及其 " + std::to_string(n) + " 条条目（记得保存）");
            if (im->cfg.loaded && !im->cfg.path.empty())
                (void)SaveConfig(im->cfg.path, im->cfg);
            JVal d = JVal::obj();
            d.set("removedEntries", JVal(n));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "combo.move") {
            const int w = p.optInt("weapon", -1);
            const std::string name = p.optStr("name");
            const int dir = p.optInt("dir", 0);
            if (name.empty()) return ErrJson("默认组合固定在最前，不能移动");
            if (dir != -1 && dir != 1) return ErrJson("dir 只能是 -1 或 1");

            // 持久化列表排序（GUI 下拉与 DLL 切换都以它为准）。
            // 老配置可能没登记 name：先补录到末尾再定位，保证索引安全。
            // 注意：name 是 const，不能改它；"目标邻居组合"先存到 other，
            // 交换后 other 就是 name 的新邻居（搬条目参照它）。
            std::string other;
            {
                std::vector<std::string>& lst = im->cfg.comboList[w];
                int at = -1;
                for (std::size_t k = 0; k < lst.size(); ++k) if (lst[k] == name) { at = (int)k; break; }
                if (at < 0) { lst.push_back(name); at = (int)lst.size() - 1; }
                const int j = at + dir;
                if (j < 0 || j >= (int)lst.size()) return ErrJson("已经到头了");
                other = lst[j];
                std::swap(lst[at], lst[j]);
            }

            std::vector<SoundEntry> mine;
            for (std::size_t i = 0; i < im->cfg.entries.size();) {
                if (im->cfg.entries[i].weaponType == w && im->cfg.entries[i].combo == name) {
                    mine.push_back(im->cfg.entries[i]);
                    im->cfg.entries.erase(im->cfg.entries.begin() + i);
                } else {
                    ++i;
                }
            }
            // 组内条目顺序 = entries 里的出现顺序，所以"移动组合"就是把这批条目整段搬到
            // 目标组合之前/之后 —— 不用另存一份组合顺序表（旧版也是这个做法）。
            std::size_t ins = im->cfg.entries.size();
            if (dir < 0) {
                for (std::size_t k = 0; k < im->cfg.entries.size(); ++k)
                    if (im->cfg.entries[k].weaponType == w && im->cfg.entries[k].combo == other) {
                        ins = k;
                        break;
                    }
            } else {
                for (std::size_t k = im->cfg.entries.size(); k-- > 0;)
                    if (im->cfg.entries[k].weaponType == w && im->cfg.entries[k].combo == other) {
                        ins = k + 1;
                        break;
                    }
            }
            im->cfg.entries.insert(im->cfg.entries.begin() + ins, mine.begin(), mine.end());
            im->dirty = true;
            im->SetStatus("已调整组合顺序（影响下拉与游戏内 Ctrl+F11 的循环顺序，记得保存）");
            if (im->cfg.loaded && !im->cfg.path.empty())
                (void)SaveConfig(im->cfg.path, im->cfg);
            JVal d = JVal::obj();
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "combo.setActive") {
            const int w = p.optInt("weapon", -1);
            const std::string name = p.optStr("name");
            if (w < 0 || w > 13) return ErrJson("weapon 越界");
            if (!name.empty()) {
                std::vector<std::string> combos = im->WeaponCombos(w);
                bool okc = false;
                for (std::size_t i = 0; i < combos.size(); ++i)
                    if (combos[i] == name) { okc = true; break; }
                if (!okc) return ErrJson("组合「" + name + "」不属于这把武器");
            }
            im->cfg.active[w] = name;
            im->dirty = true;
            // ★ 组合切换立即写盘：不点"保存"重开界面也会保持上次选的组合。
            //   SaveConfig 写的是 cfg 的当前内容；前端编辑器的草稿还没进 cfg，
            //   所以不会把"没点确定"的条目改动带出来，安全。
            if (im->cfg.loaded && !im->cfg.path.empty())
                (void)SaveConfig(im->cfg.path, im->cfg);
            JVal d = JVal::obj();
            d.set("active", JVal(name));
            return OkJson(d);
        }

        // ---------------- 音效文件 ----------------
        if (method == "sound.pick") {
            bool cancelled = false;
            const std::string rel = PickSoundFile(corePtr, owner, cancelled);
            JVal d = JVal::obj();
            d.set("path", JVal(rel));
            d.set("cancelled", JVal(cancelled || rel.empty()));
            return OkJson(d);
        }

        if (method == "sound.pickMulti") {
            int added = 0;
            JVal specs = BrowseSounds(corePtr, owner, added);
            JVal paths = JVal::arr();
            for (std::size_t i = 0; i < specs.a.size(); ++i)
                paths.push(JVal(specs.a[i].optStr("path")));
            JVal d = JVal::obj();
            d.set("paths", paths);
            d.set("specs", specs);
            d.set("cancelled", JVal(added == 0));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "sound.preview") {
            const std::string rel = p.optStr("path");
            if (rel.empty()) return ErrJson("缺少 path");
            const int vol = p.optInt("vol", 100);
            const int delay = p.optInt("delay", 0);
            const std::wstring full = ResolveSoundPath(corePtr, rel);
            // 增益 = 主音量 × 该音效倍率（和游戏内播放的算法一致，试听才是"所见即所得"）
            float gain = (im->cfg.global.volume / 100.0f) * ((vol < 0 ? 0 : (vol > 100 ? 100 : vol)) / 100.0f);
            if (gain < 0.0f) gain = 0.0f;
            if (gain > 1.0f) gain = 1.0f;
            PlayPreviewFile(full, gain, delay > 0 ? (unsigned)delay : 0);
            JVal d = JVal::obj();
            d.set("playing", JVal(true));
            d.set("resolved", JVal(ToSlash(Utf8FromWide(full))));
            return OkJson(d);
        }

        if (method == "sound.openSoundsDir" || method == "sound.openDir") {
            std::wstring sd = Utf8ToWide(im->BaseDir() + "sounds");
            const std::wstring legacy = Utf8ToWide(im->exeDir + "sounds");
            if (::GetFileAttributesW(sd.c_str()) == INVALID_FILE_ATTRIBUTES &&
                ::GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES)
                sd = legacy;                                  // 旧布局：别让用户以为音效丢了
            ::CreateDirectoryW(sd.c_str(), nullptr);
            ::ShellExecuteW(OwnerOf(owner), L"open", sd.c_str(), nullptr, nullptr, SW_SHOW);
            JVal d = JVal::obj();
            d.set("dir", JVal(ToSlash(Utf8FromWide(sd))));
            return OkJson(d);
        }

        if (method == "sound.exists") {
            const std::string rel = p.optStr("path");
            const std::wstring full = ResolveSoundPath(corePtr, rel);
            JVal d = JVal::obj();
            d.set("exists", JVal(!full.empty() &&
                                 ::GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES));
            d.set("resolved", JVal(ToSlash(Utf8FromWide(full))));
            return OkJson(d);
        }

        // ---------------- FSM / LMT 知识库 ----------------
        if (method == "fsm.search") {
            const std::string q = p.optStr("q");
            const int wf = p.optInt("weapon", -1);
            std::vector<FsmDbEntry> rows = SearchFsmDb(q, wf);
            JVal arr = JVal::arr();
            for (std::size_t i = 0; i < rows.size(); ++i) {
                JVal r = JVal::obj();
                r.set("name", JVal(rows[i].name));
                r.set("weapon", JVal(rows[i].weapon));
                r.set("weaponName", JVal(rows[i].weapon >= 0
                                             ? std::string(WeaponName(rows[i].weapon))
                                             : std::string("通用")));
                r.set("lmt", JVal(rows[i].lmt));
                r.set("fsm", JVal(rows[i].fsm));
                r.set("source", JVal(rows[i].weapon >= 0 ? "库" : "通用"));
                r.set("added", JVal(im->IsCapturedAdded(rows[i].weapon, rows[i].fsm, rows[i].lmt)));
                arr.push(r);
            }
            JVal d = JVal::obj();
            d.set("rows", arr);
            d.set("total", JVal((int)arr.a.size()));
            d.set("dbCount", JVal((int)GetFsmDb().size()));
            return OkJson(d);
        }

        if (method == "fsm.exportMeasured") {
            // 汇总"实测到的动作 ID"：实时捕获历史 + 当前配置条目（按 weapon/fsm/lmt 去重）
            std::vector<FsmDbEntry> ids;
            for (std::size_t i = 0; i < im->derHist.size(); ++i) {
                const Impl::HistEntry& h = im->derHist[i];
                if (h.fsm <= 0 && h.lmt <= 0) continue;             // 跳过自由态(0/-1)
                bool dup = false;
                for (std::size_t k = 0; k < ids.size(); ++k)
                    if (ids[k].weapon == h.weapon && ids[k].fsm == h.fsm && ids[k].lmt == h.lmt) {
                        dup = true;
                        break;
                    }
                if (dup) continue;
                FsmDbEntry e;
                e.weapon = h.weapon;
                e.fsm = h.fsm;
                e.lmt = h.lmt;
                const std::string nm = im->ResolveName(h.weapon, h.fsm, h.lmt);
                e.name = nm.empty() ? std::string("(未命名)") : nm;
                ids.push_back(e);
            }
            for (std::size_t i = 0; i < im->cfg.entries.size(); ++i) {
                const SoundEntry& se = im->cfg.entries[i];
                if (se.fsmId <= 0 && se.LmtAny() <= 0) continue;
                bool dup = false;
                for (std::size_t k = 0; k < ids.size(); ++k)
                    if (ids[k].weapon == se.weaponType && ids[k].fsm == se.fsmId &&
                        ids[k].lmt == se.LmtAny()) { dup = true; break; }
                if (dup) continue;
                FsmDbEntry e;
                e.weapon = se.weaponType;
                e.fsm = se.fsmId;
                e.lmt = se.LmtAny();
                e.name = se.name.empty() ? std::string("(未命名)") : se.name;
                ids.push_back(e);
            }

            // 1) 收进"用户库"（永不被更新覆盖），立刻生效
            const int added = MergeFsmDbEntries(ids, FsmDbUserPath());
            ReloadFsmDb();
            im->EnrichNames();
            // 2) 另存一份提交用 CSV（可直接分享 / 提交）
            SaveFsmDbCsv(FsmDbSubmissionPath(), ids);
            im->SetStatus("实测ID 共 " + std::to_string(ids.size()) + " 条：用户库新增 " +
                          std::to_string(added) + " 条 -> " + FsmDbUserPath());
            JVal d = JVal::obj();
            d.set("path", JVal(ToSlash(FsmDbUserPath())));
            d.set("submissionPath", JVal(ToSlash(FsmDbSubmissionPath())));
            d.set("count", JVal((int)ids.size()));
            d.set("added", JVal(added));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "fsm.importCsv") {
            std::vector<std::string> one;
            const std::string path = OpenFileDialog(
                owner, L"CSV 动作ID库 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0",
                nullptr, nullptr, false, one);
            if (path.empty()) {
                JVal d = JVal::obj();
                d.set("added", JVal(0));
                d.set("cancelled", JVal(true));
                return OkJson(d);
            }
            const int added = MergeFsmDbCsv(path, FsmDbUserPath());   // 别人的 csv 也进用户库
            ReloadFsmDb();
            im->EnrichNames();
            if (added > 0) im->SetStatus("已合并 " + std::to_string(added) + " 条新动作ID -> " + FsmDbUserPath());
            else           im->SetStatus("没有可合并的新条目（都已存在）");
            JVal d = JVal::obj();
            d.set("added", JVal(added));
            d.set("path", JVal(ToSlash(path)));
            d.set("userPath", JVal(ToSlash(FsmDbUserPath())));
            d.set("dbCount", JVal((int)GetFsmDb().size()));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "fsm.submit") {
            // 先导出，再把 CSV 内容放进剪贴板、打开 GitHub 提交页
            JVal ex = JVal::obj();
            {
                std::string r = Handle("fsm.exportMeasured", "{}");
                (void)r;
            }
            const std::string path = FsmDbSubmissionPath();
            std::string content;
            ReadWideFile(LongPathW(path), content);
            if (!content.empty()) {
                // 剪贴板：换行统一成 CRLF，粘进 GitHub 输入框不会挤成一行
                std::string cf;
                cf.reserve(content.size() + 16);
                for (std::size_t i = 0; i < content.size(); ++i) {
                    if (content[i] == '\n' && (i == 0 || content[i - 1] != '\r')) cf += '\r';
                    cf += content[i];
                }
                if (::OpenClipboard(OwnerOf(owner))) {
                    ::EmptyClipboard();
                    const std::wstring w = Utf8ToWide(cf);
                    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
                    HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (hg) {
                        void* dst = ::GlobalLock(hg);
                        if (dst) {
                            std::memcpy(dst, w.c_str(), bytes);
                            ::GlobalUnlock(hg);
                            ::SetClipboardData(CF_UNICODETEXT, hg);
                        } else {
                            ::GlobalFree(hg);
                        }
                    }
                    ::CloseClipboard();
                }
            }
            ::ShellExecuteW(OwnerOf(owner), L"open", kSubmitIssueUrl, nullptr, nullptr, SW_SHOWNORMAL);
            im->SetStatus("已写出 " + path + "，内容已复制到剪贴板；请在打开的页面粘贴提交");
            JVal d = JVal::obj();
            d.set("path", JVal(ToSlash(path)));
            d.set("url", JVal("https://github.com/2749478981/WeaponSoundEnhance/issues/new"));
            d.set("copied", JVal(!content.empty()));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "fsm.fetchLatest") {
            const std::wstring dst = LongPathW(FsmDbPath());
            const HRESULT hr = ::URLDownloadToFileW(nullptr, kSharedFsmDbUrl, dst.c_str(), 0, nullptr);
            if (FAILED(hr)) {
                const std::string msg = "获取最新ID库失败（检查网络或代理）";
                im->SetStatus(msg);
                return ErrJson(msg);
            }
            ReloadFsmDb();
            im->EnrichNames();
            im->SetStatus("已更新共享动作ID库 -> " + FsmDbPath());
            JVal d = JVal::obj();
            d.set("path", JVal(ToSlash(FsmDbPath())));
            d.set("count", JVal((int)GetFsmDb().size()));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "fsm.captured") {
            JVal arr = JVal::arr();
            for (std::size_t i = 0; i < im->derHist.size(); ++i) {
                const Impl::HistEntry& h = im->derHist[i];
                if (h.fsm <= 0 && h.lmt <= 0) continue;
                JVal o = JVal::obj();
                o.set("weapon", JVal(h.weapon));
                o.set("weaponName", JVal(std::string(WeaponName(h.weapon))));
                o.set("fsm", JVal(h.fsm));
                o.set("lmt", JVal(h.lmt));
                o.set("time", JVal(h.time));
                o.set("name", JVal(im->ResolveName(h.weapon, h.fsm, h.lmt)));
                o.set("added", JVal(im->IsCapturedAdded(h.weapon, h.fsm, h.lmt)));
                arr.push(o);
            }
            JVal d = JVal::obj();
            d.set("captured", arr);
            d.set("total", JVal((int)arr.a.size()));
            d.set("userPath", JVal(ToSlash(FsmDbUserPath())));
            d.set("basePath", JVal(ToSlash(FsmDbPath())));
            d.set("submissionPath", JVal(ToSlash(FsmDbSubmissionPath())));
            d.set("dbCount", JVal((int)GetFsmDb().size()));
            return OkJson(d);
        }

        // ---------------- 组合包分享 ----------------
        if (method == "combo.export") {
            const int w = p.optInt("weapon", im->prefs.weaponFilter);
            if (w < 0 || w > 13) return ErrJson("先选一把武器（当前: 全部/通用）");
            const std::string combo = im->ActiveCombo(w);
            const std::string comboName = combo.empty() ? "默认" : combo;

            std::string fname = "Sonar_" + std::string(WeaponName(w)) + "_" + comboName;
            for (std::size_t i = 0; i < fname.size(); ++i) {
                const char c = fname[i];
                if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                    c == '\\' || c == '|' || c == '?' || c == '*') fname[i] = '_';
            }
            fname += ".zip";

            const std::string zipPath = SaveFileDialog(
                owner, L"组合包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0", L"zip",
                Utf8ToWide(fname));
            if (zipPath.empty()) {
                JVal d = JVal::obj();
                d.set("cancelled", JVal(true));
                return OkJson(d);
            }

            // 暂存目录：先把音效复制到 <暂存>\sounds\，再打进 zip，收尾删掉
            const std::string stage = TempSubDir("wse_export");
            DeleteTree(Utf8ToWide(stage));
            MakeDirs(Utf8ToWide(stage));

            std::vector<SoundEntry> exported;
            int copied = 0, missing = 0;
            PackComboToDir(corePtr, w, combo, stage, exported, copied, missing);
            if (exported.empty()) {
                DeleteTree(Utf8ToWide(stage));
                return ErrJson("这个组合一条条目都没有，没什么可导出的");
            }
            std::string txt, err;
            if (!BuildComboExportText(w, combo, exported, txt, err)) {
                DeleteTree(Utf8ToWide(stage));
                return ErrJson("导出失败: " + err);
            }
            std::string txtName = fname;
            txtName.resize(txtName.size() - 4);      // 去掉 .zip
            txtName += ".txt";                       // zip 里同名换后缀
            if (!ZipBuildArchive(LongPathW(zipPath), txtName, txt, Utf8ToWide(stage + "sounds\\"), err)) {
                DeleteTree(Utf8ToWide(stage));
                return ErrJson("导出失败: " + err);
            }
            DeleteTree(Utf8ToWide(stage));

            im->SetStatus("已导出组合包（zip）: " + zipPath + "（" + std::to_string(exported.size()) +
                          " 条条目，音效 " + std::to_string(copied) + " 个）");
            JVal d = JVal::obj();
            d.set("zip", JVal(ToSlash(zipPath)));
            d.set("copied", JVal(copied));
            d.set("missing", JVal(missing));
            d.set("entries", JVal((int)exported.size()));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "combo.import") {
            std::vector<std::string> one;
            const std::string sp = OpenFileDialog(
                owner, L"组合文件 (*.zip;*.txt;*.ini)\0*.zip;*.txt;*.ini\0所有文件 (*.*)\0*.*\0",
                nullptr, nullptr, false, one);
            if (sp.empty()) {
                JVal d = JVal::obj();
                d.set("added", JVal(0));
                d.set("cancelled", JVal(true));
                return OkJson(d);
            }
            const std::string low = ToLowerAscii(sp);
            const bool isZip = low.size() > 4 && low.substr(low.size() - 4) == ".zip";
            if (!isZip) {
                JVal r = MergeConfigFile(corePtr, sp, true);
                r.set("path", JVal(ToSlash(sp)));
                return OkJson(r);
            }
            // zip：解包到临时目录，取出组合 txt 再走普通合并（旁边的 sounds\ 会自动复制进本地）
            const std::string ext = TempSubDir("wse_import");
            DeleteTree(Utf8ToWide(ext));
            MakeDirs(Utf8ToWide(ext));
            const std::string found = ZipExtractTo(LongPathW(sp), ext);
            if (found.empty()) {
                DeleteTree(Utf8ToWide(ext));
                JVal d = JVal::obj();
                d.set("added", JVal(0));
                d.set("error", JVal(std::string("这个 zip 里没有组合文件（*.txt / *.ini）")));
                return ErrJson("这个 zip 里没有组合文件（*.txt / *.ini）");
            }
            JVal r = MergeConfigFile(corePtr, found, true);
            DeleteTree(Utf8ToWide(ext));
            r.set("zip", JVal(ToSlash(sp)));
            return OkJson(r);
        }

        // ---------------- 游戏交互 ----------------
        if (method == "game.reload") {
            // ★ 重载前先把当前改动保存进 ini（点击重载 = 保存 + 生效，一步到位）
            if (im->cfg.loaded && !im->cfg.path.empty())
                (void)SaveConfig(im->cfg.path, im->cfg);
            im->dirty = false;
            // 游戏内插件轮询到这个文件就立刻重载（等价 /wse reload）
            FsWrite(im->BaseDir() + "_wse_reload.flag", "1");
            im->SetStatus("已保存 ini 并请求游戏重载配置（游戏内会立即生效；顺便把正在播的音效停掉）");
            JVal d = JVal::obj();
            d.set("flag", JVal(ToSlash(im->BaseDir() + "_wse_reload.flag")));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "game.stop") {
            FsWrite(im->BaseDir() + "_wse_stop.flag", "1");
            im->SetStatus("已请求停止游戏内正在播放的音效（等价 /wse stop）");
            JVal d = JVal::obj();
            d.set("flag", JVal(ToSlash(im->BaseDir() + "_wse_stop.flag")));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "game.launch") {
            std::string gdir;
            if (!FindGameDirFromSelf(gdir)) {
                return ErrJson("找不到 MonsterHunterWorld.exe"
                               "（GUI 需放在游戏的 nativePC\\plugins\\WeaponSoundEnhance\\ 下）");
            }
            // 已经在跑就把它切到前台：再启动一次 MHW 只会弹"游戏已在运行"
            HWND h = ::FindWindowW(nullptr, L"MONSTER HUNTER: WORLD");
            if (!h) h = ::FindWindowW(L"MonsterHunterWorld", nullptr);
            JVal d = JVal::obj();
            if (h) {
                ::ShowWindow(h, SW_RESTORE);
                ::SetForegroundWindow(h);
                im->SetStatus("怪物猎人已在运行，已切换到游戏窗口");
                d.set("launched", JVal(false));
                d.set("running", JVal(true));
                d.set("status", JVal(im->status));
                return OkJson(d);
            }
            const std::wstring exe = LongPathW(gdir + "MonsterHunterWorld.exe");
            const HINSTANCE r = ::ShellExecuteW(OwnerOf(owner), L"open", exe.c_str(), nullptr,
                                                Utf8ToWide(gdir).c_str(), SW_SHOWNORMAL);
            const bool ok = ((INT_PTR)r > 32);
            im->SetStatus(ok ? ("已启动怪物猎人：" + gdir + "MonsterHunterWorld.exe")
                             : ("启动失败，请手动运行 " + gdir + "MonsterHunterWorld.exe"));
            d.set("launched", JVal(ok));
            d.set("running", JVal(ok));
            d.set("dir", JVal(ToSlash(gdir)));
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        // ---------------- 武器触发开关（每武器一个，默认全关）----------------
        // ---------------- 武器触发开关（每武器一个，默认全关；存 cfg → SaveConfig 保留）----------------
        if (method == "weapons.enabled.get") {
            std::vector<int> en(14, 0);
            std::vector<std::string> toks;
            SplitCsv(im->cfg.global.weaponEnabled, toks);
            for (int i = 0; i < 14; ++i)
                en[i] = (i < (int)toks.size() && std::atoi(toks[i].c_str()) != 0);
            JVal arr = JVal::arr();
            for (int i = 0; i < 14; ++i) arr.push(JVal(en[i]));
            JVal d = JVal::obj();
            d.set("enabled", arr);
            return OkJson(d);
        }

        if (method == "weapons.enabled.set") {
            const JVal* je = p.find("enabled");
            if (!je || !je->isArr() || je->a.empty()) return ErrJson("缺少 enabled 数组");
            std::string line;
            for (std::size_t i = 0; i < 14; ++i) {
                if (i) line += ",";
                line += (i < je->a.size() && je->a[i].asInt(0) != 0) ? "1" : "0";
            }
            im->cfg.global.weaponEnabled = line;
            if (im->cfg.loaded && !im->cfg.path.empty()) (void)SaveConfig(im->cfg.path, im->cfg);
            FsWrite(im->BaseDir() + "_wse_reload.flag", "1");
            im->SetStatus("武器触发开关已保存并请求游戏重载（立即生效）");
            JVal d = JVal::obj();
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        if (method == "bank.play") {
            // ★ GUI 内播放：抽 wem → vgmstream-cli 解码到内存(标准 wav) → Windows PlaySound
            //   （不调用任何外部播放器）
            const std::string path = Trim(p.optStr("path"));
            const long long media = p.find("media") ? p.find("media")->asInt(0) : 0;
            if (path.empty() || media <= 0) return ErrJson("缺少 path/media");
            bankmod::BankFile bf;
            std::string err;
            if (!bankmod::LoadBank(Utf8ToWide(path), bf, err)) return ErrJson(err);
            std::vector<uint8_t> wem;
            if (!bankmod::ExtractWem(bf, (uint32_t)media, wem)) {
                // 这条 media 不在当前导入的 nbnk 里 —— 用真值表查出它真正属于哪个 bank
                // （日志里的 bank= 是"事件所属 bank"，不代表 media 的归属，别拿它当依据）
                const std::unordered_map<int, std::pair<std::string, int>>::const_iterator sit =
                    im->wemSeqs.find((int)media);
                std::string msg = "当前 nbnk（" + bf.name + "）里没有这条 media";
                if (sit != im->wemSeqs.end())
                    msg += "；它属于 " + sit->second.first + " 第 " + std::to_string(sit->second.second) + " 个";
                else
                    msg += "，也不在已收录的 nbnk 表里";
                return ErrJson(msg);
            }

            // vgmstream-cli.exe 随包分发（wemkit\vgmstream\）；开发目录兜底 tools\vgmstream
            std::wstring cli = Utf8ToWide(im->exeDir) + L"wemkit\\vgmstream\\vgmstream-cli.exe";
            if (::GetFileAttributesW(cli.c_str()) == INVALID_FILE_ATTRIBUTES)
                cli = Utf8ToWide(im->exeDir) + L"tools\\vgmstream\\vgmstream-cli.exe";
            if (::GetFileAttributesW(cli.c_str()) == INVALID_FILE_ATTRIBUTES)
                return ErrJson("缺少 wemkit\\vgmstream\\vgmstream-cli.exe（随包分发的 wem 解码器）");

            std::wstring tmpDir = Utf8ToWide(im->exeDir) + L"wemkit\\tmp";
            ::CreateDirectoryW(tmpDir.c_str(), nullptr);
            wchar_t num[32] = {};
            ::swprintf_s(num, L"%lld", media);
            const std::wstring wemFile = tmpDir + L"\\play_" + num + L".wem";
            {
                HANDLE h = ::CreateFileW(wemFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) return ErrJson("无法写临时 wem");
                DWORD wr0 = 0;
                ::WriteFile(h, wem.data(), (DWORD)wem.size(), &wr0, nullptr);
                ::CloseHandle(h);
            }

            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE rd = nullptr, wr = nullptr;
            if (!::CreatePipe(&rd, &wr, &sa, 1 << 20)) return ErrJson("创建管道失败");
            ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = wr;
            si.hStdError = wr;
            PROCESS_INFORMATION pi{};
            std::wstring cmd = L"\"" + cli + L"\" -p \"" + wemFile + L"\"";
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(0);
            const BOOL ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                             CREATE_NO_WINDOW, nullptr, tmpDir.c_str(), &si, &pi);
            ::CloseHandle(wr);
            std::vector<unsigned char> wav;
            if (ok) {
                ::CloseHandle(pi.hThread);
                unsigned char chunk[65536];
                DWORD got = 0;
                while (::ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) && got > 0)
                    wav.insert(wav.end(), chunk, chunk + got);
                ::WaitForSingleObject(pi.hProcess, 15000);
                ::CloseHandle(pi.hProcess);
            }
            ::CloseHandle(rd);
            if (wav.size() < 44) return ErrJson("wem 解码失败（vgmstream 没有输出 wav）");

            ::PlaySoundW(nullptr, nullptr, 0);      // 停掉上一次
            im->playBuf.swap(wav);                  // 缓冲区要在播放期间保活
            const BOOL played = ::PlaySoundW((LPCWSTR)im->playBuf.data(), nullptr,
                                             SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
            JVal d = JVal::obj();
            d.set("played", JVal(played != FALSE));
            d.set("bytes", JVal((int)im->playBuf.size()));
            d.set("media", JVal(media));
            return OkJson(d);
        }

        // 播放任意捕获到的 wem。
        //   候选 bank 顺序（每个都必须用 DIDX 校验"这条 media 真的在里面"）：
        //     ① 调用方给的 bankPath ② 调用方给的 bank 名（捕获行带的是 DLL 日志里
        //     事件真正播放的 bank）③ 真值表 media→bank 名 ④ 全量 DIDX 索引（优先 nativePC）
        //   为什么要校验：同一条 media id 可能出现在多个 bank/版本里（本地化语音、
        //   nativePC 的 mod 版本），不校验就会播出"另一个版本"的音效（用户报过）。
        if (method == "wem.play") {
            const long long media = p.find("media") ? p.find("media")->asInt(0) : 0;
            if (media <= 0) return ErrJson("缺少 media");
            const std::string wantBank = Trim(p.optStr("bank"));
            const std::string explicitPath = Trim(p.optStr("bankPath"));

            std::vector<std::string> cands;
            std::vector<std::string> candNames;
            auto pushCand = [&](const std::string& path, const std::string& name) {
                if (path.empty()) return;
                for (std::size_t i = 0; i < cands.size(); ++i) if (cands[i] == path) return;
                cands.push_back(path);
                candNames.push_back(name);
            };
            if (!explicitPath.empty()) pushCand(explicitPath, "指定文件");
            if (!wantBank.empty()) pushCand(im->FindBankByName(wantBank), wantBank);
            {
                const std::unordered_map<int, std::pair<std::string, int>>::const_iterator sit =
                    im->wemSeqs.find((int)media);
                if (sit != im->wemSeqs.end())
                    pushCand(im->FindBankByName(sit->second.first), sit->second.first + "（真值表）");
            }

            std::string tried;
            auto tryPlay = [&](const std::string& path, const std::string& label,
                               std::string& playedBank, std::string& err) -> bool {
                bankmod::BankFile bf;
                if (!bankmod::LoadBank(Utf8ToWide(path), bf, err)) return false;
                bool has = false;
                for (std::size_t i = 0; i < bf.media.size(); ++i)
                    if ((long long)bf.media[i].id == media) { has = true; break; }
                if (!has) {
                    err = bf.name + " 里没有这条 media";
                    if (!tried.empty()) tried += "；";
                    tried += label + "(" + bf.name + ")";
                    return false;
                }
                std::vector<uint8_t> wem;
                if (!bankmod::ExtractWem(bf, (uint32_t)media, wem)) { err = "抽取 wem 失败"; return false; }
                if (!im->PlayWemBytes(wem, media, err)) return false;
                playedBank = bf.name;
                return true;
            };

            std::string playedBank, lastErr;
            for (std::size_t i = 0; i < cands.size(); ++i) {
                std::string err;
                if (tryPlay(cands[i], candNames[i], playedBank, err)) {
                    JVal d = JVal::obj();
                    d.set("played", JVal(true));
                    d.set("media", JVal(media));
                    d.set("bank", JVal(playedBank));
                    d.set("path", JVal(ToSlash(cands[i])));
                    d.set("foundBy", JVal(candNames[i]));
                    return OkJson(d);
                }
                lastErr = err;
            }

            // ④ 兜底：全量 DIDX 索引（已按 nativePC 优先）
            im->BuildMediaBankIndex();
            const std::unordered_map<int, std::string>::const_iterator mit = im->mediaBankIndex.find((int)media);
            if (mit != im->mediaBankIndex.end()) {
                std::string err;
                if (tryPlay(mit->second, "DIDX 索引", playedBank, err)) {
                    JVal d = JVal::obj();
                    d.set("played", JVal(true));
                    d.set("media", JVal(media));
                    d.set("bank", JVal(playedBank));
                    d.set("path", JVal(ToSlash(mit->second)));
                    d.set("foundBy", JVal(std::string("DIDX 索引")));
                    return OkJson(d);
                }
                lastErr = err;
            }

            std::string msg = "找不到包含这条 media 的 nbnk 文件";
            if (!tried.empty()) msg += "（试过：" + tried + "）";
            if (!lastErr.empty()) msg += "：" + lastErr;
            return ErrJson(msg);
        }

        if (method == "bank.stopPlay") {
            ::PlaySoundW(nullptr, nullptr, 0);
            im->playBuf.clear();
            JVal d = JVal::obj();
            d.set("stopped", JVal(true));
            return OkJson(d);
        }

        // ---------------- wem 场景过滤（仅任务中触发 / 地图白名单）----------------
        if (method == "wem.scene.get") {
            JVal d = JVal::obj();
            d.set("onlyQuest", JVal(im->cfg.global.wemOnlyInQuest != 0));
            d.set("mapWhite", JVal(im->cfg.global.wemMapWhite));
            d.set("mapBlack", JVal(im->cfg.global.wemMapBlack));
            return OkJson(d);
        }

        if (method == "wem.scene.save") {
            im->cfg.global.wemOnlyInQuest = p.optBool("onlyQuest", false) ? 1 : 0;
            im->cfg.global.wemMapWhite = Trim(p.optStr("mapWhite"));
            im->cfg.global.wemMapBlack = Trim(p.optStr("mapBlack"));
            if (im->cfg.loaded && !im->cfg.path.empty()) (void)SaveConfig(im->cfg.path, im->cfg);
            FsWrite(im->BaseDir() + "_wse_reload.flag", "1");
            im->SetStatus("场景过滤已保存并请求游戏重载。当前地图 id 看游戏日志 state 行的 map=xxx");
            JVal d = JVal::obj();
            d.set("status", JVal(im->status));
            return OkJson(d);
        }

        // ---------------- 在线更新 ----------------
        if (method == "update.check") {
            const std::string user = p.optStr("user");
            const bool userInitiated = !user.empty() && user != "0" && user != "false";
            if (im->updState.load() != 1) {
                im->updState.store(1);
                {
                    std::lock_guard<std::mutex> lk(im->updMx);
                    im->updErr.clear();
                    im->updMsg = "正在检查…";
                }
                // ini 里的代理/开关（首次检查前可能还没加载配置，用当前值即可）
                const std::string proxy = im->cfg.global.updateProxy;
                const std::string cur = kWseGuiVersion;
                // ★ 后台线程：WinHTTP 超时最长 20 秒，在主线程查会把界面冻住。
                //   结果放成员 + 原子量，PollEvents 里作为 {"event":"update"} 报进度。
                Impl* imp = im;
                std::thread([imp, proxy, cur]() {
                    wseupd::Latest r = wseupd::FetchLatest(proxy);
                    if (!r.ok) {
                        std::lock_guard<std::mutex> lk(imp->updMx);
                        imp->updErr = r.err.empty() ? std::string("检查失败") : r.err;
                        imp->updMsg.clear();
                        imp->updProgress.store(-1);
                        imp->updState.store(4);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(imp->updMx);
                        imp->updTag = r.tag;
                        imp->updUrl = r.zipUrl;
                        imp->updNotes = r.notes;
                        imp->updMsg.clear();
                    }
                    const int cmp = wseupd::CompareVer(r.tag, cur);
                    // ★ 版本号相同也可能有热修复包：比较"仓库资产上传时间"与
                    //   "本地 exe 构建时间"（留 10 分钟容差：既容忍时钟误差，也避免"刚装完就提示更新"）。
                    bool sameVerNewer = false;
                    if (cmp == 0 && !r.assetUpdated.empty()) {
                        const unsigned long long remote = Impl::ParseIsoUtc(r.assetUpdated);
                        const unsigned long long local = Impl::ExeWriteTimeUtc();
                        if (remote > 0 && local > 0 && remote > local + 600) sameVerNewer = true;   // 10 分钟容差
                    }
                    imp->updSameVer.store(sameVerNewer);
                    imp->updState.store((cmp > 0 || sameVerNewer) ? 3 : 2);
                }).detach();
            }
            (void)userInitiated;
            JVal d = im->UpdateData();
            d.set("started", JVal(im->updState.load() == 1));
            return OkJson(d);
        }

        if (method == "update.install") {
            if (im->updInstalling.load()) {
                JVal d = JVal::obj();
                d.set("started", JVal(false));
                d.set("error", JVal(std::string("正在安装中")));
                return OkJson(d);
            }
            std::string url;
            {
                std::lock_guard<std::mutex> lk(im->updMx);
                url = im->updUrl;
            }
            if (url.empty()) return ErrJson("没有可下载的组合包地址（release 里没有 zip 资产）");
            im->updInstalling.store(true);
            im->updNeedRestart.store(false);
            gUpdProgress.store(0);
            im->updProgress.store(0);
            {
                std::lock_guard<std::mutex> lk(im->updMx);
                im->updMsg = "正在下载…";
                im->updErr.clear();
            }

            const std::string proxy = im->cfg.global.updateProxy;
            const std::string dataDir = im->BaseDir();
            const std::string pluginsDir = dataDir + "..\\";
            Impl* imp = im;
            // ★ 后台线程：下载动辄几十兆，必须在主线程之外做；
            //   进度经 gUpdProgress（原子量）→ PollEvents 里转成 update 事件。
            std::thread([imp, url, proxy, dataDir, pluginsDir]() {
                std::string err;
                std::wstring tmpZip = Utf8ToWide(dataDir + "_update_tmp.zip");
                std::wstring wurl = wseupd::Widen(url);
                std::wstring host, path;
                bool https = true;
                {
                    std::wstring u = wurl;
                    if (u.rfind(L"https://", 0) == 0) { u = u.substr(8); https = true; }
                    else if (u.rfind(L"http://", 0) == 0) { u = u.substr(7); https = false; }
                    const std::size_t sl = u.find(L'/');
                    host = (sl == std::wstring::npos) ? u : u.substr(0, sl);
                    path = (sl == std::wstring::npos) ? L"/" : u.substr(sl);
                }
                imp->updState.store(1);
                if (!wseupd::HttpGetFile(host, path, proxy, https, tmpZip, err, &UpdProgressCb)) {
                    std::lock_guard<std::mutex> lk(imp->updMx);
                    imp->updMsg = "下载失败：" + err;
                    imp->updProgress.store(-1);
                    imp->updInstalling.store(false);
                    imp->updState.store(4);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(imp->updMx);
                    imp->updMsg = "正在安装…";
                }
                std::string bytes;
                if (!ReadWideFile(tmpZip, bytes)) {
                    std::lock_guard<std::mutex> lk(imp->updMx);
                    imp->updMsg = "读取下载文件失败";
                    imp->updInstalling.store(false);
                    imp->updState.store(4);
                    ::DeleteFileW(tmpZip.c_str());
                    return;
                }
                mz_zip_archive z{};
                if (mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0) == MZ_FALSE) {
                    std::lock_guard<std::mutex> lk(imp->updMx);
                    imp->updMsg = "下载的 zip 无法解析";
                    imp->updInstalling.store(false);
                    imp->updState.store(4);
                    ::DeleteFileW(tmpZip.c_str());
                    return;
                }
                const std::string kPlug = "nativePC/plugins/";
                const std::string kData = "nativePC/plugins/WeaponSoundEnhance/";
                int done = 0, failed = 0, dllBlocked = 0;
                // ★ 游戏在运行时 DLL 被占用：不能静默失败，要明确告诉用户"插件没换成"
                const bool gameRunning = imp->live.attached || imp->liveOk;
                bool needRestart = false;
                const mz_uint num = mz_zip_reader_get_num_files(&z);
                for (mz_uint i = 0; i < num; ++i) {
                    mz_zip_archive_file_stat st;
                    if (mz_zip_reader_file_stat(&z, i, &st) == MZ_FALSE) continue;
                    std::string name = st.m_filename;
                    if (name.empty()) continue;
                    for (std::size_t k = 0; k < name.size(); ++k) if (name[k] == '\\') name[k] = '/';
                    if (name[name.size() - 1] == '/') continue;   // 目录项

                    std::string dst;
                    bool isGuiExe = false;
                    if (name.rfind(kData, 0) == 0) {
                        std::string rel = name.substr(kData.size());
                        if (rel == "WeaponSoundEnhance.ini") {
                            // 包里带 ini：不直接覆盖，落到临时文件，主线程再"合并"进用户配置
                            std::size_t l2 = 0;
                            void* b2 = mz_zip_reader_extract_to_heap(&z, i, &l2, 0);
                            if (b2) {
                                if (WriteWideFile(Utf8ToWide(dataDir + "_update_incoming.ini"), b2, l2))
                                    imp->updIniPending.store(true);
                                mz_free(b2);
                            }
                            continue;
                        }
                        if (rel == "WeaponSoundEnhanceGUI.exe") {
                            isGuiExe = true;
                            rel = "WeaponSoundEnhanceGUI.new.exe";   // 自己替换自己：先落 .new
                        }
                        dst = dataDir + rel;
                    } else if (name.rfind(kPlug, 0) == 0) {
                        std::string rel = name.substr(kPlug.size());
                        if (rel.find('/') != std::string::npos) continue;   // plugins\ 下只放 DLL
                        dst = pluginsDir + rel;
                    } else if (name == "WeaponSoundEnhance.dll") {
                        // 旧包结构兜底：zip 根就是 plugins\ 的内容（早期从 plugins\ 开始打包的版本）
                        dst = pluginsDir + "WeaponSoundEnhance.dll";
                    } else if (name.rfind("WeaponSoundEnhance/", 0) == 0) {
                        std::string rel = name.substr(std::strlen("WeaponSoundEnhance/"));
                        if (rel == "WeaponSoundEnhance.ini") continue;   // 旧包不带 ini
                        if (rel == "WeaponSoundEnhanceGUI.exe") {
                            isGuiExe = true;
                            rel = "WeaponSoundEnhanceGUI.new.exe";
                        }
                        dst = dataDir + rel;
                    } else {
                        continue;                                          // 其它路径忽略
                    }
                    std::size_t len = 0;
                    void* buf = mz_zip_reader_extract_to_heap(&z, i, &len, 0);
                    if (!buf) { ++failed; continue; }
                    const std::wstring wdst = Utf8ToWide(dst);
                    const std::size_t slash = wdst.find_last_of(L'\\');
                    if (slash != std::wstring::npos) MakeDirs(wdst.substr(0, slash + 1));
                    // 插件 DLL：游戏正在运行时不替换（会被占用），只计数并稍后明确提示
                    const bool isPluginDll = (dst.find("WeaponSoundEnhance.dll") != std::string::npos);
                    bool ok = false;
                    if (isPluginDll && gameRunning) {
                        ++dllBlocked;
                    } else {
                        ok = WriteWideFile(wdst, buf, len);
                    }
                    mz_free(buf);
                    if (ok) { ++done; if (isGuiExe) needRestart = true; }
                    else if (!isPluginDll || !gameRunning) ++failed;
                }
                mz_zip_reader_end(&z);
                ::DeleteFileW(tmpZip.c_str());

                imp->updNeedRestart.store(needRestart);
                imp->updInstalling.store(false);
                if (dllBlocked > 0) {
                    // 只更新了数据文件、没换 DLL：必须说清楚，否则用户以为更新成功
                    std::lock_guard<std::mutex> lk(imp->updMx);
                    imp->updMsg = "已更新 " + std::to_string(done) + " 个文件；但插件 DLL 未替换（游戏正在运行，文件被占用）。"
                                  "请关闭游戏后重新点「立即更新」，或手动解压覆盖插件 DLL。";
                    imp->updProgress.store(-1);
                    imp->updState.store(3);   // 仍显示"可更新"，让用户能重试
                    return;
                }
                if (done == 0) {
                    // ★ 一个文件都没装成功不能算成功（以前会显示"已更新 0 个文件"并当作最新，
                    //   把"包里结构不认"这类问题藏起来了）。
                    std::lock_guard<std::mutex> lk(imp->updMx);
                    imp->updMsg = (failed > 0)
                        ? std::string("安装失败：文件被占用（游戏/GUI 正在运行？）。可关掉游戏后重试。")
                        : std::string("安装包结构不认识（没有任何文件被写入）。请到发布页手动下载覆盖。");
                    imp->updProgress.store(-1);
                    imp->updState.store(4);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(imp->updMx);
                    imp->updMsg = "已更新 " + std::to_string(done) + " 个文件" +
                                  (failed ? ("（" + std::to_string(failed) + " 个失败，可能被占用）") : "") +
                                  (needRestart ? "；点下方按钮重启 GUI 完成替换" : "；重启游戏生效");
                }
                imp->updProgress.store(-1);
                imp->updState.store(2);   // 装完就等于最新了
                if (needRestart) {
                    const std::string cmdPath = dataDir + "_wse_apply_update.cmd";
                    std::string cmd =
                        "@echo off\r\n"
                        ":wait\r\n"
                        "tasklist /FI \"IMAGENAME eq WeaponSoundEnhanceGUI.exe\" 2>nul | find /I \"WeaponSoundEnhanceGUI.exe\" >nul\r\n"
                        "if not errorlevel 1 ( ping -n 2 127.0.0.1 >nul & goto wait )\r\n"
                        "move /Y \"%~dp0WeaponSoundEnhanceGUI.new.exe\" \"%~dp0WeaponSoundEnhanceGUI.exe\" >nul\r\n"
                        "start \"\" \"%~dp0WeaponSoundEnhanceGUI.exe\"\r\n"
                        "del \"%~f0\"\r\n";
                    FsWrite(cmdPath, cmd);
                }
            }).detach();

            JVal d = JVal::obj();
            d.set("started", JVal(true));
            return OkJson(d);
        }

        if (method == "update.state") {
            return OkJson(im->UpdateData());
        }

        if (method == "update.openLatest") {
            std::string url = kReleasesPage;
            {
                std::lock_guard<std::mutex> lk(im->updMx);
                if (!im->updUrl.empty()) url = kReleasesPage;   // 发布页比直链更适合人看
            }
            ::ShellExecuteW(OwnerOf(owner), L"open", Utf8ToWide(url).c_str(), nullptr, nullptr,
                            SW_SHOWNORMAL);
            JVal d = JVal::obj();
            d.set("url", JVal(url));
            return OkJson(d);
        }

        if (method == "update.restart") {
            const std::string cmd = im->BaseDir() + "_wse_apply_update.cmd";
            if (!FsExists(cmd)) {
                JVal d = JVal::obj();
                d.set("restarted", JVal(false));
                d.set("error", JVal(std::string("没找到重启脚本，请手动关闭并重新打开 GUI")));
                return OkJson(d);
            }
            ::ShellExecuteW(OwnerOf(owner), L"open", Utf8ToWide(cmd).c_str(), nullptr, nullptr, SW_HIDE);
            JVal d = JVal::obj();
            d.set("restarted", JVal(true));
            d.set("cmd", JVal(ToSlash(cmd)));
            return OkJson(d);
        }

        // ---------------- 界面偏好 ----------------
        if (method == "ui.get") {
            return OkJson(im->UiPrefsJson());
        }

        // 用系统默认浏览器打开外链（彩蛋里的 B 站链接走这里，不让 WebView 导航离开）
        if (method == "ui.openUrl") {
            const std::string url = Trim(p.optStr("url"));
            if (url.empty()) return ErrJson("缺少 url");
            const int r = (int)(std::intptr_t)::ShellExecuteW(
                nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (r <= 32) return ErrJson("打开链接失败（错误码 " + std::to_string(r) + "）");
            JVal d = JVal::obj();
            d.set("opened", JVal(true));
            return OkJson(d);
        }

        if (method == "ui.set") {
            // 只认已知的键：前端乱塞字段不该污染偏好文件
            if (p.has("colWidths")) {
                const JVal* v = p.find("colWidths");
                if (v && v->isObj()) im->prefs.colWidths = *v;
            }
            if (p.has("sort")) {
                const JVal* v = p.find("sort");
                if (v && v->isObj()) im->prefs.sort = *v;
            }
            if (p.has("window")) {
                const JVal* v = p.find("window");
                if (v && v->isObj()) im->prefs.window = *v;
            }
            if (p.has("weaponFilter")) {
                const int w = p.optInt("weaponFilter", -1);
                im->prefs.weaponFilter = (w < -2 || w > 13) ? -1 : w;
            }
            if (p.has("histExpanded")) im->prefs.histExpanded = p.optBool("histExpanded", false);
            if (p.has("onlyActive")) im->prefs.onlyActive = p.optBool("onlyActive", false);
            if (p.has("wemView")) im->prefs.wemView = p.optBool("wemView", false);
            if (p.has("theme")) {
                const std::string th = p.optStr("theme");
                if (!IsValidThemeId(th)) return ErrJson("未知主题: " + th);
                im->prefs.theme = th;
            }
            im->SaveUiPrefs();      // 立即落盘：窗口尺寸这类偏好丢了用户会很明显地不爽
            return OkJson(im->UiPrefsJson());
        }

        if (method == "theme.set") {
            const std::string th = Trim(p.optStr("theme"));
            if (!IsValidThemeId(th)) return ErrJson("未知主题: " + th);
            im->prefs.theme = th;
            im->SaveUiPrefs();
            JVal d = JVal::obj();
            d.set("theme", JVal(th));
            d.set("themes", ThemesJson());
            return OkJson(d);
        }

        if (method == "theme.get") {
            JVal d = JVal::obj();
            d.set("theme", JVal(im->prefs.theme));
            d.set("default", JVal(std::string(kDefaultTheme)));
            d.set("themes", ThemesJson());
            return OkJson(d);
        }

        // ---------------- 附加：实时捕获面板要用的（前端 index.html 用到了）----------------
        if (method == "live.get" || method == "live.retry") {
            if (method == "live.retry") {
                im->game.Detach();      // 强制下次 PollEvents 重新 Attach（游戏刚启动时用得上）
                im->lastPoll = 0;
                im->liveOk = false;
            }
            return OkJson(im->LiveJson());
        }

        if (method == "live.clear") {
            // kind="wem"：只清 wem 捕获（nbnk 制作页的清空按钮），派生历史保留；
            // 其它（默认）：全部清空。
            const std::string scope = p.optStr("scope").empty()
                ? (p.optStr("kind") == "wem" ? std::string("wem") : std::string("all"))
                : p.optStr("scope");
            const bool doWem = (scope == "wem" || scope == "all");
            const bool doDer = (scope == "derived" || scope == "all");
            const bool doEfx = (scope == "efx" || scope == "all");
            if (doDer) { im->derHist.clear(); ++im->derRev; }
            if (doWem) { im->wemHist.clear(); ++im->wemRev; }
            if (doEfx) { im->efxHist.clear(); ++im->efxRev; }
            im->curWemMedia = -1;
            im->curWemName.clear();
            im->curWemBank.clear();
            im->curWemWeapon = -1;
            im->lastWemMedia = -1;   // ★ 必须和 curWemMedia 一致：设成 -2 会让"有变化"
                                     //   永远成立 → 每 100ms 推一次 live，界面被反复重建
            im->hadLive = false;     // ★ 反过来：强制下一次轮询推一次 live，
                                     //   否则清空后若状态没变，前端一直收不到更新（像"捕获不到"）
            // ★ 循环音判定表也要清：否则清空前累积的计数/静默期会继续压住新记录
            im->wemLoops.clear();
            im->wemOkCount = 0;
            im->wemZeroCount = 0;
            // ★ 用户要的"清空"= 把 wem 日志也清掉（DLL 每次都是 OPEN_ALWAYS+APPEND 写，
            //   可以安全清空；下次写入会自动重建内容）。
            if (!im->wemLogPath.empty()) {
                HANDLE h = ::CreateFileW(Utf8ToWide(im->wemLogPath).c_str(), GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
            }
            im->wemStream.close();   // 关流：下次自动重开并跳到末尾
            // 特效捕获：同样清日志 + 统计（scope=efx 或 all）
            if (doEfx) {
                im->efxOkCount = 0;
                im->efxStats.clear();
                im->efxRawSample.clear();
                im->efxRawCount = 0;
                im->efxDup = 0;
                im->curEfxTable.clear();
                im->curEfxGroup = -1;
                im->curEfxRec = -1;
                im->curEfxWeapon = -1;
                im->curEfxPath.clear();
                if (!im->efxLogPath.empty()) {
                    HANDLE he = ::CreateFileW(Utf8ToWide(im->efxLogPath).c_str(), GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                              TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (he != INVALID_HANDLE_VALUE) ::CloseHandle(he);
                }
                im->efxStream.close();
                im->efxLastPoll = 0;
            }
            im->wemLastPoll = 0;
            JVal d = JVal::obj();
            d.set("cleared", JVal(true));
            d.set("scope", JVal(scope));
            d.set("der", im->DerJson());
            d.set("wem", im->WemJson());
            d.set("efx", im->EfxJson());
            return OkJson(d);
        }
        // ---- 用户自定义 wem 名字（保存到 WseWemNames_user.txt，下次自动读取）----
        if (method == "wem.name.get") {
            const long long media = p.find("media") ? p.find("media")->asInt(0) : 0;
            JVal d = JVal::obj();
            d.set("media", JVal(media));
            const std::unordered_map<int, std::string>::const_iterator it = im->wemNames.find((int)media);
            d.set("name", JVal(it != im->wemNames.end() ? it->second : std::string()));
            d.set("custom", JVal(im->wemUserNames.find((int)media) != im->wemUserNames.end()));
            return OkJson(d);
        }

        if (method == "wem.name.set") {
            const long long media = p.find("media") ? p.find("media")->asInt(0) : 0;
            const std::string name = Trim(p.optStr("name"));
            if (media <= 0) return ErrJson("缺少 media");
            if (name.empty()) im->wemUserNames.erase((int)media);   // 空 = 删除自定义名
            else               im->wemUserNames[(int)media] = name;
            std::string err;
            if (!im->SaveWemUserNames(err)) return ErrJson(err);
            im->ReloadWemNames();                                   // 重建（删掉后回退到官方名）
            JVal d = JVal::obj();
            const std::unordered_map<int, std::string>::const_iterator it = im->wemNames.find((int)media);
            d.set("name", JVal(it != im->wemNames.end() ? it->second : std::string()));
            d.set("custom", JVal(im->wemUserNames.find((int)media) != im->wemUserNames.end()));
            d.set("file", JVal(ToSlash(im->wemUserNamePath)));
            d.set("count", JVal((int)im->wemUserNames.size()));
            return OkJson(d);
        }

        if (method == "wem.name.openDir") {
            // 打开用户名字库所在目录（插件数据目录）
            const std::wstring dir = Utf8ToWide(im->BaseDir());
            ::CreateDirectoryW(dir.c_str(), nullptr);
            ::ShellExecuteW(OwnerOf(owner), L"open", dir.c_str(), nullptr, nullptr, SW_SHOW);
            JVal d = JVal::obj();
            d.set("dir", JVal(ToSlash(im->BaseDir())));
            return OkJson(d);
        }

        if (method == "wem.name.list") {
            JVal arr = JVal::arr();
            std::vector<std::pair<int, std::string> > v(im->wemUserNames.begin(), im->wemUserNames.end());
            std::sort(v.begin(), v.end());
            for (std::size_t i = 0; i < v.size(); ++i) {
                JVal o = JVal::obj();
                o.set("media", JVal(v[i].first));
                o.set("name", JVal(v[i].second));
                arr.push(o);
            }
            JVal d = JVal::obj();
            d.set("items", arr);
            d.set("file", JVal(ToSlash(im->wemUserNamePath)));
            return OkJson(d);
        }

        // ---- nbnk 导出的输出目录 ----
        if (method == "bank.outDirToGame") {
            // 把输出目录一键设为游戏读取 nbnk 的位置：<游戏>\nativePC\sound\wwise\Windows
            std::string gdir;
            if (!FindGameDirFromSelf(gdir)) return ErrJson("找不到游戏目录（GUI 需放在游戏的 nativePC\\plugins\\WeaponSoundEnhance\\ 下）");
            std::string dir = gdir + "nativePC\\sound\\wwise\\Windows";
            const std::wstring wd = Utf8ToWide(dir);
            ::CreateDirectoryW(wd.c_str(), nullptr);
            if (::GetFileAttributesW(wd.c_str()) == INVALID_FILE_ATTRIBUTES)
                return ErrJson("无法创建目录：" + dir);
            im->cfg.global.bankOutDir = dir;
            if (im->cfg.loaded && !im->cfg.path.empty()) (void)SaveConfig(im->cfg.path, im->cfg);
            JVal d = JVal::obj();
            d.set("dir", JVal(ToSlash(dir)));
            return OkJson(d);
        }

        if (method == "bank.outDir") {
            std::string dir = im->cfg.global.bankOutDir;
            if (dir.empty()) dir = im->BaseDir() + "wemmod";
            // 游戏读取 nbnk 的目录：用来判断"当前输出目录是不是已经指向游戏目录"，
            // 界面上把"设为游戏目录"按钮显示成"已是游戏目录"并禁用
            std::string gameDir;
            {
                std::string g;
                if (FindGameDirFromSelf(g)) gameDir = g + "nativePC\\sound\\wwise\\Windows";
            }
            JVal d = JVal::obj();
            d.set("dir", JVal(ToSlash(dir)));
            d.set("custom", JVal(!im->cfg.global.bankOutDir.empty()));
            d.set("gameDir", JVal(ToSlash(gameDir)));
            d.set("isGameDir", JVal(!gameDir.empty() && ToSlash(dir) == ToSlash(gameDir)));
            return OkJson(d);
        }

        if (method == "bank.pickOutDir") {
            std::string cur = im->cfg.global.bankOutDir;
            if (cur.empty()) cur = im->BaseDir() + "wemmod";
            const std::string picked = PickFolder(owner, Utf8ToWide(cur));
            if (!picked.empty()) {
                im->cfg.global.bankOutDir = picked;
                if (im->cfg.loaded && !im->cfg.path.empty()) (void)SaveConfig(im->cfg.path, im->cfg);
            }
            JVal d = JVal::obj();
            d.set("dir", JVal(ToSlash(im->cfg.global.bankOutDir.empty()
                                      ? (im->BaseDir() + "wemmod")
                                      : im->cfg.global.bankOutDir)));
            d.set("changed", JVal(!picked.empty()));
            return OkJson(d);
        }

        if (method == "bank.openOut") {
            std::string dir = im->cfg.global.bankOutDir;
            if (dir.empty()) dir = im->BaseDir() + "wemmod";
            const std::wstring wd = Utf8ToWide(dir);
            ::CreateDirectoryW(wd.c_str(), nullptr);
            ::ShellExecuteW(OwnerOf(owner), L"open", wd.c_str(), nullptr, nullptr, SW_SHOW);
            JVal d = JVal::obj();
            d.set("dir", JVal(ToSlash(dir)));
            return OkJson(d);
        }

        // 播放本地音频文件（替换用的 wav/mp3/ogg）：vgmstream 解码到内存 → PlaySound
        if (method == "audio.playFile") {
            const std::string path = Trim(p.optStr("path"));
            if (path.empty()) return ErrJson("缺少 path");
            const std::wstring src = Utf8ToWide(path);
            if (::GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
                return ErrJson("文件不存在：" + path);
            std::wstring cli = Utf8ToWide(im->exeDir) + L"wemkit\\vgmstream\\vgmstream-cli.exe";
            if (::GetFileAttributesW(cli.c_str()) == INVALID_FILE_ATTRIBUTES)
                cli = Utf8ToWide(im->exeDir) + L"tools\\vgmstream\\vgmstream-cli.exe";
            if (::GetFileAttributesW(cli.c_str()) == INVALID_FILE_ATTRIBUTES)
                return ErrJson("缺少 wemkit\\vgmstream\\vgmstream-cli.exe");
            std::wstring tmpDir = Utf8ToWide(im->exeDir) + L"wemkit\\tmp";
            ::CreateDirectoryW(tmpDir.c_str(), nullptr);

            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE rd = nullptr, wr = nullptr;
            if (!::CreatePipe(&rd, &wr, &sa, 1 << 20)) return ErrJson("创建管道失败");
            ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = wr;
            si.hStdError = wr;
            PROCESS_INFORMATION pi{};
            std::wstring cmd = L"\"" + cli + L"\" -p \"" + src + L"\"";
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(0);
            const BOOL ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                             CREATE_NO_WINDOW, nullptr, tmpDir.c_str(), &si, &pi);
            ::CloseHandle(wr);
            std::vector<unsigned char> wav;
            if (ok) {
                ::CloseHandle(pi.hThread);
                unsigned char chunk[65536];
                DWORD got = 0;
                while (::ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) && got > 0)
                    wav.insert(wav.end(), chunk, chunk + got);
                ::WaitForSingleObject(pi.hProcess, 15000);
                ::CloseHandle(pi.hProcess);
            }
            ::CloseHandle(rd);
            if (wav.size() < 44) return ErrJson("解码失败（格式不支持或文件损坏）");
            ::PlaySoundW(nullptr, nullptr, 0);
            im->playBuf.swap(wav);
            const BOOL played = ::PlaySoundW((LPCWSTR)im->playBuf.data(), nullptr,
                                             SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
            JVal d = JVal::obj();
            d.set("played", JVal(played != FALSE));
            d.set("bytes", JVal((int)im->playBuf.size()));
            return OkJson(d);
        }

        if (method == "bank.pick") {
            std::vector<std::string> multi;
            const std::string p1 = OpenFileDialog(OwnerOf(owner),
                L"Wwise SoundBank (*.nbnk)\0*.nbnk\0所有文件 (*.*)\0*.*\0", nullptr, L"nbnk",
                false, multi);
            JVal d = JVal::obj();
            d.set("cancelled", JVal(p1.empty()));
            d.set("path", JVal(ToSlash(p1)));
            return OkJson(d);
        }

        if (method == "audio.pickMulti") {
            std::vector<std::string> multi;
            const std::string p1 = OpenFileDialog(OwnerOf(owner),
                L"音频文件 (*.wav;*.mp3;*.ogg;*.flac)\0*.wav;*.mp3;*.ogg;*.flac\0"
                L"所有文件 (*.*)\0*.*\0", nullptr, L"wav", true, multi);
            JVal arr = JVal::arr();
            if (!p1.empty()) arr.push(JVal(ToSlash(p1)));
            for (std::size_t i = 0; i < multi.size(); ++i) arr.push(JVal(ToSlash(multi[i])));
            JVal d = JVal::obj();
            d.set("files", arr);
            d.set("cancelled", JVal(arr.a.empty()));
            return OkJson(d);
        }

        if (method == "bank.inspect") {
            const std::string path = Trim(p.optStr("path"));
            if (path.empty()) return ErrJson("缺少 path");
            bankmod::BankFile bf;
            std::string err;
            if (!bankmod::LoadBank(Utf8ToWide(path), bf, err)) return ErrJson(err);
            JVal arr = JVal::arr();
            for (std::size_t i = 0; i < bf.media.size(); ++i) {
                const bankmod::MediaEntry& m = bf.media[i];
                JVal o = JVal::obj();
                o.set("id", JVal((long long)m.id));
                o.set("seq", JVal(m.seq));
                o.set("size", JVal((int)m.size));
                // 名字来自 mediaids 映射（有就有，没有就空）
                const std::unordered_map<int, std::string>::const_iterator it = im->wemNames.find((int)m.id);
                o.set("name", JVal(it != im->wemNames.end() ? it->second : std::string()));
                  // 是否用户命名（true 才把 name 当真名显示，否则前端显示 bank·第N个）
                  o.set("custom", JVal(im->wemUserNames.find((int)m.id) != im->wemUserNames.end()));
                  // 注释名（mediaids 里括号中的中文，如"瓶子声"）——界面名称列优先显示它
                  const std::unordered_map<int, std::string>::const_iterator nt = im->wemNotes.find((int)m.id);
                  o.set("note", JVal(nt != im->wemNotes.end() ? nt->second : std::string()));
                // 序号真值（真值表：id -> bank/第N个）
                const std::unordered_map<int, std::pair<std::string, int>>::const_iterator sit =
                    im->wemSeqs.find((int)m.id);
                if (sit != im->wemSeqs.end()) o.set("seqInBank", JVal(sit->second.second));
                arr.push(o);
            }
            JVal d = JVal::obj();
            d.set("bank", JVal(bf.name));
            d.set("path", JVal(ToSlash(path)));
            d.set("count", JVal((int)bf.media.size()));
            d.set("media", arr);
            d.set("hasWwise", JVal(!bankmod::FindWwiseConsole().empty()));
            return OkJson(d);
        }

        if (method == "ui.playAsset") {
            // 播放内嵌资源里的音效（成就提示音 coinmul.wav 等）
            const std::string name = Trim(p.optStr("name"));
            if (name.empty()) return ErrJson("缺少 name");
            JVal d = JVal::obj();
            d.set("played", JVal(im->PlayAsset(name)));
            return OkJson(d);
        }

        if (method == "ui.assetsDir") {
            JVal d = JVal::obj();
            d.set("dir", JVal(ToSlash(im->assetsDir)));
            return OkJson(d);
        }

        if (method == "wem.convertEnv") {
            // 转换环境自检：给界面显示"能不能转 wem"
            const std::wstring wwise = bankmod::FindWwiseConsole();
            std::wstring ff = Utf8ToWide(im->exeDir) + L"wemkit\\ffmpeg.exe";
            const bool hasFf = ::GetFileAttributesW(ff.c_str()) != INVALID_FILE_ATTRIBUTES;
            std::wstring vg = Utf8ToWide(im->exeDir) + L"wemkit\\vgmstream\\vgmstream-cli.exe";
            const bool hasVg = ::GetFileAttributesW(vg.c_str()) != INVALID_FILE_ATTRIBUTES;
            JVal d = JVal::obj();
            d.set("hasWwise", JVal(!wwise.empty()));
            d.set("wwisePath", JVal(Utf8FromWide(wwise)));
            d.set("hasFfmpeg", JVal(hasFf));
            d.set("hasVgmstream", JVal(hasVg));
            return OkJson(d);
        }

        if (method == "wem.convert") {
            // 音频文件（任意格式）→ wem（ffmpeg + WwiseConsole，本地完成）
            const JVal* jf = p.find("files");
            if (!jf || !jf->isArr() || jf->a.empty()) return ErrJson("缺少 files");
            std::vector<std::wstring> files;
            for (std::size_t i = 0; i < jf->a.size(); ++i)
                files.push_back(Utf8ToWide(jf->a[i].asStr()));
            // Wwise 工程：GUI 目录下的 wemkit\wavtowemscript.wproj（随包分发）
            std::wstring proj = Utf8ToWide(im->exeDir) + L"wemkit\\wavtowemscript\\wavtowemscript.wproj";
            if (::GetFileAttributesW(proj.c_str()) == INVALID_FILE_ATTRIBUTES)
                return ErrJson("缺少 wemkit 工程（wemkit\\wavtowemscript\\wavtowemscript.wproj）");
            // ffmpeg：wemkit\ffmpeg.exe → PATH（仅非 wav 输入才需要）
            std::wstring ff = Utf8ToWide(im->exeDir) + L"wemkit\\ffmpeg.exe";
            if (::GetFileAttributesW(ff.c_str()) == INVALID_FILE_ATTRIBUTES) ff.clear();
            std::wstring work = Utf8ToWide(im->exeDir) + L"wemkit\\tmp";
            std::vector<std::wstring> wems;
            std::string log;
            if (!bankmod::ConvertToWem(files, proj, ff, work, wems, log)) return ErrJson(log);
            JVal arr = JVal::arr();
            for (const std::wstring& w : wems) arr.push(JVal(Utf8FromWide(w)));
            JVal d = JVal::obj();
            d.set("wems", arr);
            d.set("log", JVal(log));
            return OkJson(d);
        }

        if (method == "bank.export") {
            const std::string path = Trim(p.optStr("path"));
            const std::string outDir = Trim(p.optStr("outDir"));
            if (path.empty()) return ErrJson("缺少 path");
            const JVal* jr = p.find("replacements");
            if (!jr || !jr->isArr() || jr->a.empty()) return ErrJson("还没有要替换的音效");
            bankmod::BankFile bf;
            std::string err;
            if (!bankmod::LoadBank(Utf8ToWide(path), bf, err)) return ErrJson(err);

            std::vector<std::pair<uint32_t, std::wstring>> reps;
            for (std::size_t i = 0; i < jr->a.size(); ++i) {
                const JVal& o = jr->a[i];
                const long long media = o.find("media") ? o.find("media")->asInt(0) : 0;
                const std::string wem = o.find("wem") ? o.find("wem")->asStr() : std::string();
                if (media <= 0 || wem.empty()) continue;
                reps.push_back(std::make_pair((uint32_t)media, Utf8ToWide(wem)));
            }
            if (reps.empty()) return ErrJson("替换列表为空");

            std::wstring outDirW;
            if (!outDir.empty()) outDirW = Utf8ToWide(outDir);
            else if (!im->cfg.global.bankOutDir.empty()) outDirW = Utf8ToWide(im->cfg.global.bankOutDir);
            else outDirW = Utf8ToWide(im->BaseDir()) + L"wemmod";
            ::CreateDirectoryW(outDirW.c_str(), nullptr);
            const std::wstring outPath = outDirW + L"\\" + Utf8ToWide(bf.name) + L".nbnk";
            if (!bankmod::ExportBank(bf, reps, outPath, err)) return ErrJson(err);
            JVal d = JVal::obj();
            d.set("out", JVal(ToSlash(Utf8FromWide(outPath))));
            d.set("replaced", JVal((int)reps.size()));
            return OkJson(d);
        }

        return ErrJson("未知方法: " + method);
    } catch (const std::exception& e) {
        return ErrJson(std::string("内部异常: ") + e.what());
    } catch (...) {
        return ErrJson("内部异常（未知）");
    }
}

// ===========================================================================
//  五、轮询：实时捕获事件 + 更新进度事件
// ===========================================================================
std::string Core::PollEvents() {
    try {
        if (!mImpl) return std::string();
        Impl* im = mImpl;
        void* corePtr = (void*)this;

        // ---- 在线更新：优先报送（用户正盯着进度条）----
        // 只有在状态或进度真的变了才推，避免 100ms 一次刷屏。
        {
            static int sLastState = -2;
            static int sLastProg = -3;
            static unsigned sLastRestart = 99;
            const int st = im->updState.load();
            int pr = im->updProgress.load();
            // 下载进度用下载线程里的原子量覆盖（更细）
            if (im->updInstalling.load() && im->updState.load() == 1) pr = gUpdProgress.load();
            im->updProgress.store(pr);
            const unsigned nr = im->updNeedRestart.load() ? 1u : 0u;
            const bool changed = (st != sLastState) || (pr != sLastProg) || (nr != sLastRestart);
            const bool active = (st == 1);
            if (changed && (active || st != sLastState || nr != sLastRestart)) {
                sLastState = st;
                sLastProg = pr;
                sLastRestart = nr;
                JVal ev = JVal::obj();
                ev.set("event", JVal(std::string("update")));
                ev.set("data", im->UpdateData());
                return JsonDump(ev);
            }
        }

        // ---- 更新包里的 ini：等回到主线程再合并（不替换用户配置）----
        if (im->updIniPending.load()) {
            im->updIniPending.store(false);
            const std::string inc = im->BaseDir() + "_update_incoming.ini";
            if (FsExists(inc)) {
                if (!im->cfg.loaded || im->cfg.path.empty()) {
                    // 还没有配置：直接采用这份
                    JVal inner = JVal::obj();
                    inner.set("path", JVal(inc));
                    (void)Handle("config.load", JsonDump(inner));
                    std::lock_guard<std::mutex> lk(im->updMx);
                    im->updMsg += "；已采用更新包里的 ini";
                } else {
                    const std::string before = std::to_string(im->cfg.entries.size());
                    (void)MergeConfigFile(corePtr, inc, false);   // 按条目去重合并，保留本地配置
                    (void)SaveConfig(im->cfg.path, im->cfg);
                    std::lock_guard<std::mutex> lk(im->updMx);
                    im->updMsg += "；已把更新包 ini 合并进你的配置（条目 " + before + " → " +
                                  std::to_string(im->cfg.entries.size()) + "，未替换）";
                }
                ::DeleteFileW(Utf8ToWide(inc).c_str());
            }
        }

        // ---- 实时捕获：按 ini 的 PollMs 节流采样 ----
        im->PollGame();

        // 只在"值有变化"时推送：live 事件 100ms 一次的宿主轮询下，
        // 无脑推送会让 WebView2 侧每秒重建几十次历史 DOM（卡顿的来源）。
        const bool att = im->live.attached && im->liveOk;
        const bool inScene = att && im->live.inScene;
        const int fsm = inScene ? im->live.fsm : -1;
        const int lmt = inScene ? im->live.lmt : -1;
        const int weapon = inScene ? im->live.weapon : -1;
        const int weaponId = inScene ? im->live.weaponId : -1;
        const std::string err = im->live.error.empty() ? im->game.LastError() : im->live.error;

        const bool changed = !im->hadLive || att != im->lastAttached ||
                             inScene != im->lastInScene || err != im->lastError ||
                             fsm != im->lastFsm || lmt != im->lastLmt ||
                             weapon != im->lastWeapon || weaponId != im->lastWeaponId ||
                             im->curWemMedia != im->lastWemMedia ||
                             // ★ 捕获列表变了也必须推送：以前只比 fsm/lmt/武器，站着不动时
                             //   新捕获（或游戏退出后）永远推不出去 → 界面一直显示旧数据（实测）
                             im->efxRev != im->lastEfxRev ||
                             im->wemRev != im->lastWemRev ||
                             im->derRev != im->lastDerRev;
        if (!changed) {
            // UI 偏好有改动就顺手落盘（重连/清历史这类操作不改变 live 值，
            // 但窗口尺寸等偏好不能等到退出才写），最多 1 秒写一次。
            const unsigned long long now = ::GetTickCount64();
            if (im->uiDirty && now - im->lastHistPush > 1000) {
                im->lastHistPush = now;
                im->SaveUiPrefs();
            }
            return std::string();
        }

        im->hadLive = true;
        im->lastAttached = att;
        im->lastInScene = inScene;
        im->lastError = err;
        im->lastFsm = fsm;
        im->lastLmt = lmt;
        im->lastWeapon = weapon;
        im->lastWeaponId = weaponId;
        im->lastWemMedia = im->curWemMedia;
        im->lastEfxRev = im->efxRev;
        im->lastWemRev = im->wemRev;
        im->lastDerRev = im->derRev;

        JVal ev = JVal::obj();
        ev.set("event", JVal(std::string("live")));
        ev.set("data", im->LiveJson());
        return JsonDump(ev);
    } catch (...) {
        return std::string();   // 轮询里出错就静默（下一秒还会再来），绝不让宿主崩
    }
}
