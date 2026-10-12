

// ===========================================================================
//  main_web.cpp —— Sonar 配置工具的新宿主（WebView2）
//
//  职责非常单一：开一个 Win32 窗口 → 里面塞一个 WebView2 → 把 JS 和
//  Core::Handle() 用 JSON 对接起来。所有业务逻辑都在 core.cpp 里，这里不认识
//  任何"条目""组合""音效"的概念。
//
//  通信（两个方向都走 JSON 文本，避免嵌套转义把中文搞坏）：
//    JS   → 宿主：postMessage(JSON.stringify({id, method, params}))
//    宿主 → JS  ：PostWebMessageAsString('{"id":N,"ok":true,"data":...}')
//                 前端收到后自己 JSON.parse
//    宿主 → JS（主动事件）：'{"event":"live","data":...}' —— 没有 id
//
//  资源加载：把 exe 目录用 SetVirtualHostNameToFolderMapping 映射成
//  https://sonar.local/，然后打开 https://sonar.local/web/index.html。
//  这样 web/ 里的 ../weapons_icons/、../sonar_icon.png 都在映射根之内
//  （file:// 直接开的话，相对上级目录会被浏览器拦掉）。
// ===========================================================================

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <objbase.h>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <wrl.h>
#include <gdiplus.h>

#include "../third_party/webview2/include/WebView2.h"
#include "core.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

#ifndef IDI_APP
#define IDI_APP 100
#endif

namespace {

Core                 g_core;
ULONG_PTR            g_gdiToken = 0;   // GDI+ 启动令牌（启动加载画面用）
ComPtr<ICoreWebView2Controller> g_ctrl;
ComPtr<ICoreWebView2>           g_web;
HWND                 g_hwnd = nullptr;
ICoreWebView2*          g_curSender = nullptr;    // 当前请求的发送方（浮窗也会发请求，回复必须回给它）
ICoreWebView2Environment* g_env = nullptr;   // 浮窗要用同一个 Environment
std::wstring            g_webRootW;                 // 映射根（exe 目录），浮窗控制器要自己设映射
UINT_PTR             g_pollTimer = 0;
bool                 g_ready = false;    // true = 网页已就绪可显示（前端发 ui.ready 后置位）
int                  g_animPhase = 0;    // 启动加载画面的动画帧（图标呼吸/文字点点）

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring ExeDirW() {
    wchar_t buf[MAX_PATH * 2] = {};
    ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0]) - 1));
    std::wstring p(buf);
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(0, s + 1);
}

// 启动期日志：WebView2 的初始化是异步的，出错时界面只会"白屏"，
// 没有任何提示。把每一步写进 <exeDir>sonar_gui.log，排查时一眼看到卡在哪。
// 【坑】用 "a, ccs=UTF-8" 打开时 MSVC 要求配 fwprintf，窄 fprintf 会被丢掉
// （文件建出来了但是空的）。所以这里用二进制模式，参数一律先转成 UTF-8 窄串。
void LogLine(const char* fmt, ...) {
    static std::wstring path = ExeDirW() + L"sonar_gui.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    // 这个日志只在排查启动问题（典型症状：白屏）时有用，别让它无限增长：
    // 超过 64KB 就清空重来。插件目录里多一个 1KB 的日志无所谓，长到几 MB 就很讨厌。
    if (ftell(f) > 64 * 1024) {
        fclose(f);
        f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    }
    SYSTEMTIME st; ::GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---- 极简的"信封"解析：从 {id, method, params} 里挖出三段 ----
// 不引 JSON 库：前端发的信封是我们自己拼的，格式固定，够用就行。
struct Envelope { long long id = 0; std::string method; std::string params; };

// 从 pos 开始找 key 后面那个值；返回值的起止下标
bool FindKey(const std::string& s, const std::string& key, size_t& valStart) {
    std::string pat = "\"" + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return false;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
    valStart = p;
    return p < s.size();
}

std::string ReadJsonString(const std::string& s, size_t p) {
    if (p >= s.size() || s[p] != '"') return std::string();
    std::string out;
    for (++p; p < s.size(); ++p) {
        char c = s[p];
        if (c == '\\' && p + 1 < s.size()) {
            char n = s[++p];
            switch (n) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    // 前端用 JSON.stringify 生成，中文一般原样（不转义），这里只兜底
                    if (p + 4 < s.size()) {
                        unsigned cp = 0;
                        for (int k = 1; k <= 4; ++k) {
                            char h = s[p + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        }
                        p += 4;
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: out += n; break;
            }
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

// 从 p 起取一个平衡的 JSON 值（对象/数组/字符串/字面量）
std::string ReadJsonValue(const std::string& s, size_t p) {
    if (p >= s.size()) return "{}";
    char c = s[p];
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        bool inStr = false;
        for (size_t i = p; i < s.size(); ++i) {
            char d = s[i];
            if (inStr) {
                if (d == '\\') { ++i; continue; }
                if (d == '"') inStr = false;
                continue;
            }
            if (d == '"') { inStr = true; continue; }
            if (d == open) ++depth;
            else if (d == close) { if (--depth == 0) return s.substr(p, i - p + 1); }
        }
        return "{}";
    }
    if (c == '"') {
        bool inStr = false;
        for (size_t i = p; i < s.size(); ++i) {
            char d = s[i];
            if (d == '\\') { ++i; continue; }
            if (d == '"') { if (i > p) return s.substr(p, i - p + 1); inStr = true; continue; }
        }
        return "{}";
    }
    size_t e = p;
    while (e < s.size() && s[e] != ',' && s[e] != '}') ++e;
    return s.substr(p, e - p);
}

Envelope ParseEnvelope(const std::string& s) {
    Envelope e;
    size_t p = 0;
    if (FindKey(s, "id", p)) {
        long long v = 0;
        size_t i = p;
        bool neg = false;
        if (i < s.size() && s[i] == '-') { neg = true; ++i; }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; }
        e.id = neg ? -v : v;
    }
    if (FindKey(s, "method", p)) e.method = ReadJsonString(s, p);
    if (FindKey(s, "params", p)) e.params = ReadJsonValue(s, p);
    if (e.params.empty()) e.params = "{}";
    return e;
}

void PostJson(const std::string& json) {
    if (!g_web) return;
    g_web->PostWebMessageAsString(Utf8ToWide(json).c_str());
}

// ===========================================================================
//  主窗口内部的"启动加载画面"（纯底层绘制，不弹独立方框）。
//  在 WebView 就绪前，主窗口客户区直接画大图标 + "正在启动…"；
//  WebView 一旦接管（g_ready=true）就不再画，由网页覆盖。
//  WebView 背景色已设成同底色(#F6F7F9)，所以 图标→浅色→网页 过渡无缝。
// ===========================================================================
//  ★ 双缓冲：先把整帧画到内存位图，再一次贴出 —— 否则 25fps 下每帧
//    先 FillRect 清屏再画图，会看到"一闪一闪"。
void PaintLoading(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = ::BeginPaint(hwnd, &ps);
    RECT rc;
    ::GetClientRect(hwnd, &rc);

    HDC mem = ::CreateCompatibleDC(dc);
    HBITMAP bm = ::CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
    HBITMAP obm = (HBITMAP)::SelectObject(mem, bm);

    HBRUSH bg = ::CreateSolidBrush(RGB(246, 247, 249));
    ::FillRect(mem, &rc, bg);
    ::DeleteObject(bg);

    // 图标呼吸：40ms/帧的高帧率 + 正弦曲线 → 又慢又平滑（约 4 秒一个来回）。
    const double ang = (double)(g_animPhase % 100) / 100.0 * 6.28318530718;
    const int isz = 122 + (int)(6.0 + 6.0 * std::sin(ang));   // 122..134
    const int ix = (rc.right - isz) / 2;
    const int iy = rc.top + (rc.bottom - rc.top - isz - 62) / 2;

    // ★ 用 GDI+ 从 exe 目录加载 256px 的 sonar_icon.png，再做高质量双三次缩放。
    bool drew = false;
    {
        Gdiplus::Bitmap bmp((ExeDirW() + L"sonar_icon.png").c_str());
        if (g_gdiToken && bmp.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Graphics g(mem);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.DrawImage(&bmp, ix, iy, isz, isz);
            drew = true;
        }
    }
    if (!drew) {
        HICON icon = (HICON)::LoadImageW((HINSTANCE)::GetModuleHandleW(nullptr),
                                         MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 256, 256, 0);
        ::DrawIconEx(mem, ix, iy, icon, isz, isz, 0, nullptr, DI_NORMAL);
    }

    // 文字：三个点轮转，每约 0.5s 换一个；大号雅黑，避免小字体在高 DPI 下发糊
    static const wchar_t* dots[3] = { L".", L"..", L"..." };
    wchar_t txt[64];
    wsprintfW(txt, L"Sonar 正在启动%s", dots[(g_animPhase / 13) % 3]);

    HFONT f = ::CreateFontW(-24, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    HFONT of = (HFONT)::SelectObject(mem, f);
    ::SetBkMode(mem, TRANSPARENT);
    ::SetTextColor(mem, RGB(110, 110, 115));
    RECT tr = { 8, iy + isz + 24, rc.right - 8, rc.bottom - 6 };
    ::DrawTextW(mem, txt, -1, &tr, DT_CENTER | DT_TOP);
    ::SelectObject(mem, of);
    ::DeleteObject(f);

    // 一次贴出，杜绝闪烁
    ::BitBlt(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, obm);
    ::DeleteObject(bm);
    ::DeleteDC(mem);
    ::EndPaint(hwnd, &ps);
}

// 把 Core 的 {"ok":...} 前面插一个 id，变成 {"id":N,"ok":...}
void Reply(long long id, const std::string& result) {
    std::string body = result;
    if (body.empty() || body[0] != '{') body = "{\"ok\":false,\"error\":\"core returned non-object\"}";
    const std::string msg = std::string("{\"id\":") + std::to_string(id) + "," + body.substr(1);
    if (g_curSender) g_curSender->PostWebMessageAsString(Utf8ToWide(msg).c_str());
    else PostJson(msg);   // 浮窗发的请求，回复回给浮窗（否则前端"后端无响应"超时）
}

// 浮窗（定义在文件后半段）
void ShowOverlay(bool on);
bool OverlayVisible();
void OverlaySetTopmost(bool on);
void LoadOverlayPrefs();
void PostJsonToOverlay(const std::string& json);
bool IsReadOnlyMethod(const std::string& m);   // 只读方法不广播刷新（否则 refresh 自己触发自己 → 死循环）
extern bool g_ovTopmost;   // 定义在浮窗段（下面）

void OnMessage(const std::string& text) {
    Envelope env = ParseEnvelope(text);
    if (env.method == "ui.ready") {
        // 前端数据就绪 → 显示 WebView、停掉加载动画、回个 ok 收尾
        if (!g_ready && g_ctrl) {
            g_ready = true;
            g_ctrl->put_IsVisible(TRUE);
            ::KillTimer(g_hwnd, 3);
            ::KillTimer(g_hwnd, 4);
            ::InvalidateRect(g_hwnd, nullptr, TRUE);
        }
        Reply(env.id, "{\"ok\":true,\"data\":{}}");
        return;
    }
    if (env.method.empty()) { Reply(env.id, "{\"ok\":false,\"error\":\"missing method\"}"); return; }
    std::string result;
    try {
        // ---- 捕获浮窗：由宿主自己处理（窗口/控制器都在宿主里） ----
    if (env.method.rfind("overlay.", 0) == 0) {
        if (env.method == "overlay.toggle")       ShowOverlay(!OverlayVisible());
        else if (env.method == "overlay.show")    ShowOverlay(true);
        else if (env.method == "overlay.hide")    ShowOverlay(false);
        else if (env.method == "overlay.topmost") OverlaySetTopmost(env.params.find("\"on\":true") != std::string::npos ||
                                                                   env.params.find("\"on\":1") != std::string::npos);
        result = std::string("{\"ok\":true,\"data\":{\"visible\":") + (OverlayVisible() ? "true" : "false") +
                 ",\"topmost\":" + (g_ovTopmost ? "true" : "false") + "}}";
        PostJson(std::string("{\"event\":\"overlay\",\"data\":{\"visible\":") +
                 (OverlayVisible() ? "true" : "false") + "}}");
    } else result = g_core.Handle(env.method, env.params);   // ★ 必须是 else：否则会覆盖上面 overlay 的结果
    } catch (const std::exception& ex) {
        result = std::string("{\"ok\":false,\"error\":\"") + ex.what() + "\"}";
    } catch (...) {
        result = "{\"ok\":false,\"error\":\"unknown exception in core\"}";
    }
    Reply(env.id, result);
    // ★ 一端改了数据 → 通知另一端刷新（浮窗里「＋ 添加」后主界面要同步，反之亦然）
    static unsigned long long s_lastBcast = 0;
    const unsigned long long nowB = ::GetTickCount64();
    if (!IsReadOnlyMethod(env.method) && nowB - s_lastBcast > 500) {   // 500ms 防抖：即使漏了某个方法也不会无限互刷
        s_lastBcast = nowB;
        const std::string ev = "{\"event\":\"refresh\"}";
        if (g_curSender && g_curSender != g_web.Get()) PostJson(ev);   // 浮窗发的 → 通知主窗口
        else PostJsonToOverlay(ev);                              // 主窗口发的 → 通知浮窗
    }
      // 诊断：后端失败时留日志（排查"操作失败"类提示用）
      if (result.find("\"ok\":false") != std::string::npos)
          LogLine("backend err: %s | %s", env.method.c_str(), result.c_str());
}

// ---------------------------------------------------------------------------
//  WebView2 初始化
// ---------------------------------------------------------------------------
typedef HRESULT(STDAPICALLTYPE* PFN_CreateEnv)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

// 加载器 DLL 必须在 exe 旁边；用动态加载，这样缺 DLL 时能给一句人话而不是启动失败
PFN_CreateEnv LoadWebView2Entry() {
    std::wstring dll = ExeDirW() + L"WebView2Loader.dll";
    HMODULE h = ::LoadLibraryW(dll.c_str());
    if (!h) h = ::LoadLibraryW(L"WebView2Loader.dll");   // 再试 PATH
    if (!h) return nullptr;
    return (PFN_CreateEnv)::GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions");
}

// 递归删目录（清 WebView2 缓存用）。失败就算了，不影响功能。
void RemoveDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd = {};
    HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirRecursive(p);
            } else {
                ::SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(p.c_str());
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    ::RemoveDirectoryW(dir.c_str());
}

// ★ WebView2 会把 index.html / css / js 缓存到磁盘。
//   资源改了它却继续用旧的，表现出来就是"我明明改了 CSS，界面一点没变"。
//   但每次启动都清空缓存又会让首屏明显变慢（每次都要重新读盘）。
//   折中：拿 web\index.html 的「大小 + 修改时间」当指纹，只在不一致时清一次。
//   这样升级后绝不会用到旧界面，平时启动也不再重复读盘。
void ClearWebViewCache(const std::wstring& userDataDir) {
    RemoveDirRecursive(userDataDir + L"\\EBWebView\\Default\\Cache");
    RemoveDirRecursive(userDataDir + L"\\EBWebView\\Default\\Code Cache");
    RemoveDirRecursive(userDataDir + L"\\EBWebView\\Default\\GPUCache");
}

void ClearWebViewCacheIfChanged(const std::wstring& userDataDir, const std::wstring& webRoot) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    std::wstring stamp = L"missing";
    if (::GetFileAttributesExW((webRoot + L"web\\index.html").c_str(),
                               GetFileExInfoStandard, &fad)) {
        wchar_t buf[64] = {};
        ::swprintf_s(buf, L"%lu.%lu", fad.nFileSizeLow, fad.ftLastWriteTime.dwLowDateTime);
        stamp = buf;
    }
    const std::wstring marker = userDataDir + L"\\sonar_assets.txt";

    // 读上次的指纹
    std::string prev;
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, marker.c_str(), L"rb") == 0 && f) {
            char buf[128] = {};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            prev.assign(buf, n);
            fclose(f);
        }
    }
    const std::string want = WideToUtf8(stamp);
    if (prev == want) return;                    // 资源没变，缓存可以留着

    ClearWebViewCache(userDataDir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, marker.c_str(), L"wb") == 0 && f) {
        fwrite(want.data(), 1, want.size(), f);
        fclose(f);
    }
    LogLine("assets changed (%s -> %s), webview cache cleared",
            prev.empty() ? "(none)" : prev.c_str(), want.c_str());
}

std::wstring UserDataDir() {
    // 允许用环境变量覆盖（排查"数据目录被占用"这类问题时很方便）
    wchar_t ov[MAX_PATH * 3] = {};
    DWORD on = ::GetEnvironmentVariableW(L"SONAR_WV_DATA", ov, (DWORD)(sizeof(ov) / sizeof(ov[0])));
    if (on && on < sizeof(ov) / sizeof(ov[0])) {
        std::wstring d(ov);
        ::CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }
    // 放 %LOCALAPPDATA%\Sonar\WebView2：插件目录可能只读/被同步，别把缓存写进游戏目录
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring base = (n && n < sizeof(buf) / sizeof(buf[0])) ? std::wstring(buf) : ExeDirW();
    std::wstring d = base + L"\\Sonar\\WebView2";
    ::CreateDirectoryW((base + L"\\Sonar").c_str(), nullptr);
    ::CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void ResizeWebView() {
    if (!g_ctrl || !g_hwnd) return;
    RECT rc = {};
    ::GetClientRect(g_hwnd, &rc);
    g_ctrl->put_Bounds(rc);
    // 只在尺寸变化时记一行，方便对比"窗口多大 / 实际给 WebView 多大"
    static long lw = -1, lh = -1;
    const long w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w != lw || h != lh) {
        lw = w; lh = h;
        LogLine("resize: client=%ldx%ld", w, h);
    }
}

void StartPolling() {
    if (!g_pollTimer) g_pollTimer = ::SetTimer(g_hwnd, 1, 100, nullptr);
}

// 页面自检：把视口尺寸/主题/图标加载情况打进日志。
// ★ 必须等前端把数据拉回来、DOM 渲染完之后再跑 —— NavigationCompleted 触发时
//   页面里的异步 refresh() 还没回来，那时候数图标只会数到 0。
void ProbePage() {
    if (!g_web) return;
    g_web->ExecuteScript(
        L"(function(){"
        L"function R(s){var e=document.querySelector(s);if(!e)return null;var r=e.getBoundingClientRect();"
        L"return [Math.round(r.left),Math.round(r.right),Math.round(r.width)];}"
        L"var im=document.querySelectorAll('img[src*=\"weapons_icons\"]');"
        L"var ok=0;for(var i=0;i<im.length;i++){if(im[i].naturalWidth>0)ok++;}"
        L"var first=document.querySelector('img[src*=\"weapons_icons\"]');"
        L"var logo=document.getElementById('logoImg');"
        L"var api=window.SonarAPI||null;"
        L"var apiInfo=api?('ok'+(api.__error?(' ERR:'+api.__error):'')+(api.__missing&&api.__missing.length?(' MISS:'+api.__missing.join(',')):'')):'MISSING';"
        L"var hosts=document.querySelectorAll('.capHost');"
        L"var hm=document.querySelector('.capHost[data-page=\"main\"]');"
        L"var hb=document.querySelector('.capHost[data-page=\"bank\"]');"
        L"var cm=hm?hm.querySelectorAll('.hrec').length:-1;"
        L"var cb=hb?hb.querySelectorAll('.hrec').length:-1;"
        L"var vs=document.querySelectorAll('.capHost .viewswitch .vs');"
        L"var view=vs.length?((vs[0].className.indexOf('on')>=0)?'derived':'wem'):'none';"
        L"var st=document.getElementById('statusText');"
        L"var stat=document.querySelector('.capHost .capstat');"
        L"var filt=document.querySelector('.capHost .capfilter');"
        L"var ct=hm?(hm.textContent||'').replace(/\\s+/g,' ').slice(0,110):'(none)';"
        L"var wemView=(function(){"
        L"if(typeof Capture==='undefined'||!hm)return 'nocap';"
        L"var old=Capture.view;"
        L"Capture.view='wem';"
        L"var exp=hm.querySelector('.note.exp');"
        L"var rw=hm.querySelectorAll('.hrec').length;"
        L"var s=exp?('exp:'+(exp.textContent||'').slice(0,14)):'exp:MISSING';"
        L"Capture.view=old;"
        L"return s+' wemRows='+rw;})();"
        L"var layout=(function(){"
        L"var host=hm;if(!host)return 'nohost';var hist=host.querySelector('.hist');if(!hist)return 'nohist';"
        L"var d=document.createElement('div');d.className='hrec wem two';"
        L"d.innerHTML='<div class=\"l1\"><b>00:00:00</b><span class=\"n\">很长的音效名称测试一二三四五六七八九十</span></div>'"
        L"+'<div class=\"l2\"><span class=\"ids\">media 1234567890 · pl_wp_par_op_event_structure · 第2个</span>'"
        L"+'<span class=\"capacts\"><button class=\"capbtn primary\">▶ 播放</button><button class=\"capbtn\">命名</button><button class=\"capbtn\">＋ 添加</button></span></div>';"
        L"hist.appendChild(d);"
        L"var rh=Math.round(d.getBoundingClientRect().height);"
        L"var ow=hist.scrollWidth-hist.clientWidth;"
        L"var bw=Math.round(d.querySelector('.capacts').getBoundingClientRect().width);"
        L"hist.removeChild(d);"
        L"return rh+'px ovf'+ow+' btns'+bw;})();"
        L"return JSON.stringify({"
        L"w:innerWidth,h:innerHeight,theme:document.documentElement.getAttribute('data-theme'),"
        L"mainCols:getComputedStyle(document.querySelector('main')).gridTemplateColumns,"
        L"icons:ok+'/'+im.length,firstIcon:(first?first.getAttribute('src'):null),"
        L"logoOk:(logo?logo.naturalWidth>0:null),"
        L"capType:(typeof Capture),api:apiInfo,hosts:hosts.length,capRows:cm+'/'+cb,view:view,"
        L"layout:layout,wemView:wemView,capStat:(stat?stat.textContent:''),capFilter:(filt?filt.textContent:''),"
        L"capText:ct,status:(st?st.textContent:'')"
        L"});})()",
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, LPCWSTR json) -> HRESULT {
                LogLine("page: %s", WideToUtf8(json ? json : L"(null)").c_str());
                return S_OK;
            }).Get());

}


// ===========================================================================
//  捕获浮窗（overlay）：独立的置顶小窗口，只显示捕获历史
//
//  用途：游戏全屏/无边框时也能看到捕获记录。用**同一个 WebView2 Environment**
//  再开一个控制器，页面是 index.html#overlay（前端只渲染捕获面板）。
//  位置/尺寸/置顶开关记在 Sonar_Overlay.json，下次打开还原。
// ===========================================================================
HWND                    g_ovHwnd = nullptr;
ICoreWebView2Controller* g_ovCtrl = nullptr;
bool                    g_ovTopmost = true;
bool                    g_ovWantOn = false;   // 上次是否开着（启动还原）
RECT                    g_ovRect = { -1, -1, 0, 0 };   // -1 = 未初始化（首次放右上角）
std::wstring            g_ovPrefPath;

void SaveOverlayPrefs() {
    if (g_ovPrefPath.empty()) return;
    if (g_ovHwnd && ::IsWindowVisible(g_ovHwnd)) {
        RECT r{};
        ::GetWindowRect(g_ovHwnd, &r);
        g_ovRect = r;
    }
    std::string j = "{\"on\":" + std::string((g_ovHwnd && ::IsWindowVisible(g_ovHwnd)) ? "true" : "false") +
                    ",\"top\":" + std::string(g_ovTopmost ? "true" : "false") +
                    ",\"x\":" + std::to_string(g_ovRect.left) + ",\"y\":" + std::to_string(g_ovRect.top) +
                    ",\"w\":" + std::to_string(g_ovRect.right - g_ovRect.left) +
                    ",\"h\":" + std::to_string(g_ovRect.bottom - g_ovRect.top) + "}";
    HANDLE h = ::CreateFileW(g_ovPrefPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    ::WriteFile(h, j.data(), (DWORD)j.size(), &wr, nullptr);
    ::CloseHandle(h);
}

void LoadOverlayPrefs() {
    if (g_ovPrefPath.empty()) return;
    HANDLE h = ::CreateFileW(g_ovPrefPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[512] = {0};
    DWORD rd = 0;
    ::ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    ::CloseHandle(h);
    const std::string j(buf, rd);
    auto num = [&j](const char* key, long def) -> long {
        const std::string k = std::string("\"") + key + "\":";
        const std::size_t p = j.find(k);
        if (p == std::string::npos) return def;
        return std::atol(j.c_str() + p + k.size());
    };
    g_ovRect.left   = num("x", -1);
    g_ovRect.top    = num("y", -1);
    g_ovRect.right  = g_ovRect.left + num("w", 430);
    g_ovRect.bottom = g_ovRect.top + num("h", 560);
    g_ovTopmost     = j.find("\"top\":false") == std::string::npos;
    g_ovWantOn      = j.find("\"on\":true") != std::string::npos;
}

void LayoutOverlay() {
    if (!g_ovCtrl || !g_ovHwnd) return;
    RECT rc{};
    ::GetClientRect(g_ovHwnd, &rc);
    g_ovCtrl->put_Bounds(rc);
}

LRESULT CALLBACK OvWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: LayoutOverlay(); return 0;
        case WM_MOVE: case WM_EXITSIZEMOVE: SaveOverlayPrefs(); return 0;
        case WM_CLOSE: ::ShowWindow(hwnd, SW_HIDE); SaveOverlayPrefs(); return 0;   // 关 = 隐藏，下次还能开
        case WM_TIMER:
            if (wp == 11) { SaveOverlayPrefs(); return 0; }
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void CreateOverlayWindow() {
    if (g_ovHwnd) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OvWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SonarOverlayWnd";
    ::RegisterClassExW(&wc);

    if (g_ovRect.left < 0 || g_ovRect.top < 0) {           // 首次：放屏幕右上角
        RECT wa{};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        g_ovRect.right  = wa.right - 24;
        g_ovRect.left   = g_ovRect.right - 430;
        g_ovRect.top    = wa.top + 24;
        g_ovRect.bottom = g_ovRect.top + 560;
    }
    {   // 兜底：保存的位置在当前显示器之外（换显示器/分辨率变了）→ 拉回工作区右上角
        RECT wa{}; ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const long w = g_ovRect.right - g_ovRect.left, h = g_ovRect.bottom - g_ovRect.top;
        if (g_ovRect.left < wa.left - 40 || g_ovRect.top < wa.top - 40 ||
            g_ovRect.left > wa.right - 80 || g_ovRect.top > wa.bottom - 40) {
            g_ovRect.right = wa.right - 24; g_ovRect.left = g_ovRect.right - w;
            g_ovRect.top = wa.top + 24; g_ovRect.bottom = g_ovRect.top + h;
            LogLine("overlay: saved rect off-screen, moved back to %ld,%ld", g_ovRect.left, g_ovRect.top);
        }
    }
    g_ovHwnd = ::CreateWindowExW(
        g_ovTopmost ? WS_EX_TOPMOST : 0, L"SonarOverlayWnd", L"Sonar 捕获记录",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_SIZEBOX,
        g_ovRect.left, g_ovRect.top, g_ovRect.right - g_ovRect.left, g_ovRect.bottom - g_ovRect.top,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!g_ovHwnd) { LogLine("overlay: CreateWindowExW failed"); return; }
    LogLine("overlay: window created %ldx%ld topmost=%d",
            g_ovRect.right - g_ovRect.left, g_ovRect.bottom - g_ovRect.top, (int)g_ovTopmost);

    if (!g_env) return;
    g_env->CreateCoreWebView2Controller(
        g_ovHwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
                if (FAILED(hr) || !ctrl) { LogLine("overlay: controller failed hr=0x%08X", (unsigned)hr); return S_OK; }
                g_ovCtrl = ctrl;
                g_ovCtrl->AddRef();
                g_ovCtrl->put_IsVisible(TRUE);
                LayoutOverlay();
                ICoreWebView2* wv = nullptr;
                g_ovCtrl->get_CoreWebView2(&wv);
                if (!wv) { LogLine("overlay: get_CoreWebView2 failed"); return S_OK; }
                // ★ 虚拟主机映射是"每个控制器"的：浮窗必须自己再设一次，
                //   否则页面打不开（实测显示"找不到 sonar.local 的服务器 IP 地址"）。
                {
                    ComPtr<ICoreWebView2_3> wv3;
                    if (SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&wv3))) && wv3) {
                        wv3->SetVirtualHostNameToFolderMapping(L"sonar.local", g_webRootW.c_str(),
                                                              COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                        wchar_t la2[MAX_PATH * 2] = {};
                        if (::GetEnvironmentVariableW(L"LOCALAPPDATA", la2, MAX_PATH * 2) > 0) {
                            const std::wstring assets = std::wstring(la2) + L"\\Sonar\\assets";
                            wv3->SetVirtualHostNameToFolderMapping(L"sonar.assets", assets.c_str(),
                                                                  COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                        }
                        LogLine("overlay: host mappings set (sonar.local + sonar.assets)");
                    } else {
                        LogLine("overlay: ICoreWebView2_3 unavailable, mappings NOT set");
                    }
                }
                // 浮窗也要能调后端（捕获面板里的「浮窗」按钮就在浮窗页面上）
                wv->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR raw = nullptr;
                            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                g_curSender = sender;
                                OnMessage(WideToUtf8(raw));
                                g_curSender = nullptr;
                                ::CoTaskMemFree(raw);
                            }
                            return S_OK;
                        }).Get(), nullptr);
                wv->Navigate(L"https://sonar.local/web/index.html#overlay");
                // 记一条导航结果，便于排查浮窗白屏/打不开
                {
                    EventRegistrationToken tok{};
                    wv->add_NavigationCompleted(
                        Callback<ICoreWebView2NavigationCompletedEventHandler>(
                            [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                                BOOL ok = FALSE;
                                a->get_IsSuccess(&ok);
                                // 自检：浮窗模式是否生效（只显示捕获面板）
                                if (sender) sender->ExecuteScript(
                                    L"(function(){try{var h=document.querySelector('.app > header.brand');"
                                    L"var c=document.querySelector('#mainEntries > .capture');"
                                    L"var ce=document.querySelector('#mainEntries > .center');"
                                    L"var rw=document.querySelectorAll('#entryBody tr').length;"
                                    L"return 'body='+document.body.className+' header='+(h?getComputedStyle(h).display:'?')+"
                                    L"' cap='+(c?getComputedStyle(c).display:'?')+' center='+(ce?getComputedStyle(ce).display:'?')+' rows='+rw;}catch(e){return 'EXC:'+e.message;}})()",
                                    Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                                        [](HRESULT hr2, LPCWSTR json) -> HRESULT {
                                            if (SUCCEEDED(hr2)) LogLine("overlay: mode check %s", WideToUtf8(json ? json : L"(null)").c_str());
                                            return S_OK;
                                        }).Get());
                                LogLine("overlay: nav completed success=%d", (int)ok);
                                return S_OK;
                            }).Get(), &tok);
                }
                wv->Release();
                LogLine("overlay: controller ok, navigating #overlay");
                return S_OK;
            }).Get());
}

void ShowOverlay(bool on) {
    if (on) {
        CreateOverlayWindow();
        if (!g_ovHwnd) return;
        ::ShowWindow(g_ovHwnd, SW_SHOW);
        ::SetWindowPos(g_ovHwnd, g_ovTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ::SetTimer(g_ovHwnd, 11, 2000, nullptr);           // 定期记住位置
    } else if (g_ovHwnd) {
        ::KillTimer(g_ovHwnd, 11);
        ::ShowWindow(g_ovHwnd, SW_HIDE);
    }
    SaveOverlayPrefs();
    LogLine("overlay: visible=%d topmost=%d", (int)on, (int)g_ovTopmost);
}

// 把同一个事件也发给浮窗（它只显示捕获面板，需要实时数据）
// 纯读取的方法：调用它们不会改配置，广播刷新会造成 refresh ↔ refresh 死循环
bool IsReadOnlyMethod(const std::string& m) {
    // 只读 + 界面/设置类：这些不改条目数据，广播刷新只会引起"两边互相刷"的死循环（实测卡死）
    static const char* kRead[] = { "state", "live.", "fsm.", "ids.", "update.", "ui.", "name.", "wem.names",
                                   "theme.", "global.", "hotkeys.", "weapons.enabled.", "config.", "audio.",
                                   "sound.preview", "bank.play", "bank.stopPlay", "bank.inspect", "overlay." };
    return false;
}

void PostJsonToOverlay(const std::string& json) {
    if (!g_ovCtrl || !OverlayVisible()) return;
    ICoreWebView2* wv = nullptr;
    if (SUCCEEDED(g_ovCtrl->get_CoreWebView2(&wv)) && wv) {
        wv->PostWebMessageAsString(Utf8ToWide(json).c_str());
        wv->Release();
    }
}

bool OverlayVisible() { return g_ovHwnd && ::IsWindowVisible(g_ovHwnd) != FALSE; }

void OverlaySetTopmost(bool on) {
    g_ovTopmost = on;
    if (g_ovHwnd) {
        ::SetWindowPos(g_ovHwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // 重建扩展样式，保证下次显示仍是置顶
        LONG_PTR ex = ::GetWindowLongPtrW(g_ovHwnd, GWL_EXSTYLE);
        if (on) ex |= WS_EX_TOPMOST; else ex &= ~WS_EX_TOPMOST;
        ::SetWindowLongPtrW(g_ovHwnd, GWL_EXSTYLE, ex);
    }
    SaveOverlayPrefs();
}

}  // namespace

// ---------------------------------------------------------------------------
//  窗口过程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            ResizeWebView();
            return 0;
        case WM_TIMER:
            if (wp == 1) {
                std::string ev = g_core.PollEvents();
                if (!ev.empty()) { PostJson(ev); PostJsonToOverlay(ev); }   // 浮窗也要实时数据
            } else if (wp == 2) {
                // 首次自检：等前端把数据拉完、DOM 渲染好之后再量；之后每 15 秒复检一次
                // （排查"捕获到底有没有在记录"这类问题，必须能连续看到数字变化）
                ::KillTimer(hwnd, 2);
                ::SetTimer(hwnd, 5, 15000, nullptr);
                ProbePage();
            } else if (wp == 5) {
                ProbePage();
            } else if (wp == 3) {
                // 兜底：前端一直没发 ui.ready（JS 出错），8 秒后仍把 WebView 放出来，
                // 让用户至少看到界面/错误，别卡在加载画面。
                ::KillTimer(hwnd, 3);
                if (!g_ready && g_ctrl) {
                    g_ready = true;
                    g_ctrl->put_IsVisible(TRUE);
                    ::KillTimer(hwnd, 4);
                    ::InvalidateRect(hwnd, nullptr, TRUE);
                }
            } else if (wp == 4) {
                // 加载动画帧：重绘主窗口（图标呼吸/文字点点）
                g_animPhase++;
                ::InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_PAINT:
            // WebView 就绪前，主窗口画出"启动加载画面"（不弹独立方框）
            if (!g_ready) {
                PaintLoading(hwnd);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            // ★ 必须吞掉擦背景：窗口背景是白色，每帧先擦成白底再画就会一闪一闪
            return 1;
        case WM_SETFOCUS:
            if (g_ctrl) g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;
        case WM_DESTROY:
            if (g_pollTimer) { ::KillTimer(hwnd, 1); g_pollTimer = 0; }
            ::KillTimer(hwnd, 4);
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // ★ 单实例：重复双击时把已有窗口激活并退出，不弹提示框、不重复开窗。
    //   窗口类名是我们自定义的唯一名，FindWindow 很安全。
    {
        HWND w = ::FindWindowW(L"SonarConfigGUI", nullptr);
        if (w) {
            ::ShowWindow(w, SW_RESTORE);
            ::SetForegroundWindow(w);
            return 0;
        }
        // 竞态兜底：窗口可能正在创建（类已注册、窗口未建），互斥已存在就等它建完
        HANDLE mtx = ::CreateMutexW(nullptr, TRUE, L"SonarConfigGUI_SingleInstance");
        if (mtx && ::GetLastError() == ERROR_ALREADY_EXISTS) {
            ::Sleep(600);
            w = ::FindWindowW(L"SonarConfigGUI", nullptr);
            if (w) { ::ShowWindow(w, SW_RESTORE); ::SetForegroundWindow(w); }
            return 0;
        }
        // 互斥句柄故意不释放：进程生命周期内一直持有，挡住后续重复启动
    }

    // ★ DPI 感知必须最先声明。
    //   WebView2 是按"物理像素"跟宿主对账的：如果宿主是 DPI 不感知的老式进程，
    //   Windows 会做坐标虚拟化，GetClientRect 拿到的和 WebView 实际渲染尺寸对不上，
    //   页面就会按错误的视口排版 —— 表现出来就是"内容比窗口宽、右边被切掉"。
    //   v2 = 每显示器 DPI 感知（多屏不同缩放也不会错）。
    //   老系统没有这个 API 时退回 SetProcessDPIAware()。
    {
        HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
        typedef BOOL(WINAPI* PFN_SetCtx)(DPI_AWARENESS_CONTEXT);
        auto setCtx = u32 ? (PFN_SetCtx)::GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
        if (setCtx) setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        else ::SetProcessDPIAware();
    }

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LogLine("---- start (dpi aware) ----");

    // GDI+ 初始化（启动加载画面用，画高品质 PNG）
    {
        Gdiplus::GdiplusStartupInput gsi;
        Gdiplus::GdiplusStartup(&g_gdiToken, &gsi, nullptr);
    }

    // ---- 窗口类（图标沿用 resource.rc 里的 IDI_APP）----
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = (HICON)::LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)::LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                     ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)::GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"SonarConfigGUI";
    ::RegisterClassExW(&wc);

    // 默认窗口：桌面工作区的 92%，居中打开（旧版固定 1400x900，在 2K/4K 屏上偏小）
    RECT wa = { 0, 0, 1920, 1080 };
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
    int ww = (aw * 92) / 100, wh = (ah * 92) / 100;
    if (ww < 1440) ww = (aw < 1440 ? aw : 1440);
    if (wh < 900)  wh = (ah < 900 ? ah : 900);
    if (ww > aw) ww = aw;
    if (wh > ah) wh = ah;
    const int wx = wa.left + (aw - ww) / 2, wy = wa.top + (ah - wh) / 2;

    RECT wr = { 0, 0, ww, wh };
    ::AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"Sonar 配置工具",
                                  WS_OVERLAPPEDWINDOW, wx, wy,
                                  wr.right - wr.left, wr.bottom - wr.top,
                                  nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        // 以前这里直接 return 1，正是"双击毫无反应"的一种来源
        const unsigned long le = (unsigned long)::GetLastError();
        LogLine("CreateWindowExW failed, lastError=%lu", le);
        ::MessageBoxW(nullptr,
            (std::wstring(L"创建窗口失败（错误码 ") + std::to_wstring(le) + L"）。\n\n"
             L"如果持续出现，请把目录下的 sonar_gui.log 发给作者。").c_str(),
            L"Sonar 配置工具", MB_ICONERROR);
        return 1;
    }
    g_hwnd = hwnd;
    g_core.SetOwnerWindow(hwnd);
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    // ★ 主窗口此时画出"启动加载画面"（WM_PAINT 里，不等 WebView），无白屏无方框；
    //   动画定时器立刻开始：40ms/帧的高帧率，用正弦把呼吸做得又慢又平滑
    ::SetTimer(hwnd, 4, 40, nullptr);

    // ---- 启动前自检：WebView2Loader.dll / web 目录缺一不可，缺了就明确提示 ----
    {
        if (!::PathFileExistsW((ExeDirW() + L"web\\index.html").c_str())) {
            ::MessageBoxW(hwnd,
                L"缺少 web 文件夹。\n\n"
                L"界面文件必须和 WeaponSoundEnhanceGUI.exe 放在一起。\n"
                L"请把整个发布包解压到同一目录后运行，不要单独拷贝 exe。",
                L"Sonar 配置工具", MB_ICONERROR);
            return 1;
        }
        if (!::PathFileExistsW((ExeDirW() + L"weapons_icons").c_str())) {
            // 只缺图标不致命，提一声就行，不用拦着不让用
            LogLine("warning: weapons_icons folder missing, icons will show placeholders");
        }
    }

    // ---- 拿 WebView2 入口 ----
    PFN_CreateEnv createEnv = LoadWebView2Entry();
    LogLine("loader: %s", createEnv ? "ok" : "MISSING");
    if (!createEnv) {
        ::MessageBoxW(hwnd,
            L"找不到 WebView2Loader.dll。\n\n"
            L"它必须和 WeaponSoundEnhanceGUI.exe 放在同一个目录里（发布包里自带）。",
            L"Sonar 配置工具", MB_ICONERROR);
        return 1;
    }

    std::wstring dataDir = UserDataDir();
    const std::wstring webRoot = ExeDirW();          // 映射根 = exe 目录
    ClearWebViewCacheIfChanged(dataDir, webRoot);    // 别让旧 CSS/JS 赖在磁盘缓存里
    LogLine("dataDir=%s", WideToUtf8(dataDir).c_str());
    LogLine("webRoot=%s", WideToUtf8(webRoot).c_str());
    // 浮窗偏好（位置/开关）放在 exe 目录，与 ini 同级；启动时还原
    g_ovPrefPath = webRoot + L"Sonar_Overlay.json";
    LoadOverlayPrefs();

    HRESULT hr = createEnv(
        nullptr, dataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [webRoot](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
                g_env = env; if (g_env) g_env->AddRef();   // 浮窗复用同一个 Environment
                if (g_ovWantOn) { ShowOverlay(true); LogLine("overlay: restored on startup"); }
                LogLine("env handler: hr=0x%08X env=%p", (unsigned)res, (void*)env);
                if (FAILED(res) || !env) {
                    ::MessageBoxW(g_hwnd,
                        L"WebView2 初始化失败。\n\n"
                        L"请确认已安装 Microsoft Edge WebView2 Runtime"
                        L"（Win11 和大多数 Win10 自带）。",
                        L"Sonar 配置工具", MB_ICONERROR);
                    return res;
                }
                env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [webRoot](HRESULT r2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            LogLine("ctrl handler: hr=0x%08X ctrl=%p", (unsigned)r2, (void*)ctrl);
                            if (FAILED(r2) || !ctrl) {
                                // 最常见的两个原因：
                                //  - 上一个实例的 WebView2 残留进程还占着数据目录（会返回 ERROR_BUSY 0x800700AA）
                                //  - 安全软件拦截了 msedgewebview2.exe 子进程
                                wchar_t hb[16] = {};
                                ::swprintf_s(hb, L"%08X", (unsigned)r2);
                                const std::wstring msg =
                                    std::wstring(L"WebView2 初始化未完成（错误码 0x") + hb + L"）。\n\n"
                                    L"常见原因与处理：\n"
                                    L"1) 上一个窗口进程可能还在后台 → 打开任务管理器结束 "
                                    L"WeaponSoundEnhanceGUI.exe 和 msedgewebview2.exe 再试；\n"
                                    L"2) 杀毒软件拦截了 WebView2 组件 → 把本目录加入白名单再试；\n"
                                    L"3) 如果反复失败，把目录下的 sonar_gui.log 发给作者。";
                                ::MessageBoxW(g_hwnd, msg.c_str(),
                                    L"Sonar 配置工具", MB_ICONERROR);
                                return r2;
                            }
                            g_ctrl = ctrl;
                            ctrl->get_CoreWebView2(&g_web);
                            LogLine("corewebview2=%p", (void*)g_web.Get());
                            if (!g_web) {
                                ::MessageBoxW(g_hwnd,
                                    L"无法取得 WebView2 内核。\n\n请把目录下的 sonar_gui.log 发给作者。",
                                    L"Sonar 配置工具", MB_ICONERROR);
                                return E_FAIL;
                            }

                            // 禁掉右键菜单/开发者工具入口以外的杂项，保持干净
                            ComPtr<ICoreWebView2Settings> st;
                            if (SUCCEEDED(g_web->get_Settings(&st)) && st) {
                                st->put_AreDefaultContextMenusEnabled(FALSE);
                                st->put_IsStatusBarEnabled(FALSE);
                                st->put_AreDevToolsEnabled(TRUE);   // 出问题能 F12 查
                            }

                            // 把 exe 目录映射成 https://sonar.local/ —— 这样
                            // web/ 里引用的 ../weapons_icons/、../sonar_icon.png
                            // 都落在映射根内部，不会被 WebView2 拦掉。
                            // 【注意】SetVirtualHostNameToFolderMapping 在
                            // ICoreWebView2_3 上，不是 ICoreWebView2 —— 要 QueryInterface。
                            ComPtr<ICoreWebView2_3> web3;
                            if (SUCCEEDED(g_web.As(&web3)) && web3) {
                                g_webRootW = webRoot;   // 浮窗复用
                                web3->SetVirtualHostNameToFolderMapping(
                                    L"sonar.local", webRoot.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

                                // 内嵌资源（武器图标 / logo / 成就音效）：
                                // 启动时由 core 解压到 %LOCALAPPDATA%\Sonar\assets，
                                // 这里映射成 https://sonar.assets，游戏目录保持干净。
                                wchar_t la[MAX_PATH * 2] = {};
                                if (::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH * 2) > 0) {
                                    std::wstring assets = std::wstring(la) + L"\\Sonar\\assets";
                                    ::CreateDirectoryW(assets.c_str(), nullptr);
                                    web3->SetVirtualHostNameToFolderMapping(
                                        L"sonar.assets", assets.c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                    LogLine("assets host mapped: %s", WideToUtf8(assets).c_str());
                                }
                            }

                            // ★ 导航完成前 WebView 区域默认是白底，一打开会先白屏一秒，
                            //   然后加载图才出现。把 WebView 背景色设成加载图的底色
                            //   (#F6F7F9，极简浅色)，白屏阶段就变成和加载图一样的浅色，
                            //   视觉上是"打开就是加载图"，无白屏断层。
                            //   （在 ICoreWebView2Controller2 上：put_DefaultBackgroundColor）
                            {
                                ComPtr<ICoreWebView2Controller2> c2;
                                if (SUCCEEDED(g_ctrl.As(&c2)) && c2) {
                                    COREWEBVIEW2_COLOR c = { 255, 246, 247, 249 };  // A,R,G,B = #F6F7F9
                                    c2->put_DefaultBackgroundColor(c);
                                }
                            }

                            // JS → C++
                            g_web->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        g_curSender = sender;   // 回复要回给发送方
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                            OnMessage(WideToUtf8(raw));
                                            ::CoTaskMemFree(raw);
                                        } else if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                                            OnMessage(WideToUtf8(raw));
                                            ::CoTaskMemFree(raw);
                                        }
                                        g_curSender = nullptr;
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            ResizeWebView();
                            g_web->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                                        BOOL ok = FALSE;
                                        a->get_IsSuccess(&ok);
                                        COREWEBVIEW2_WEB_ERROR_STATUS es = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                        a->get_WebErrorStatus(&es);
                                        LogLine("nav completed: success=%d webErr=%d", (int)ok, (int)es);
                                        // 把页面真实视口/DPR/生效主题打出来 —— 布局或主题不对劲时，
                                        // 这一行能直接区分"是 CSS 没生效"还是"视口尺寸就不是我以为的"
                                        // 自检延后几秒：等前端异步 refresh() 把数据渲染完再量。
                                        // 给足时间 —— 首次启动（缓存刚清）时首屏会慢一些，
                                        // 太早量会数到"只有左栏、表格还没建"的中间状态。
                                        ::SetTimer(g_hwnd, 2, 7000, nullptr);
                                        return S_OK;
                                    }).Get(),
                                nullptr);
                            LogLine("navigating to https://sonar.local/web/index.html ...");
                            // ★ WebView 先隐藏：加载画面（主窗口画）保持到网页真正就绪，
                            //   前端发 ui.ready 后才 put_IsVisible(TRUE)，中间不闪空。
                            g_ctrl->put_IsVisible(FALSE);
                            g_web->Navigate(L"https://sonar.local/web/index.html");
                            // 兜底定时器：8 秒内前端没发 ui.ready 就把 WebView 放出来
                            ::SetTimer(g_hwnd, 3, 8000, nullptr);
                            g_ready = false;
                            StartPolling();   // 实时捕获 / 更新进度靠它主动推给前端
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    LogLine("createEnv hr=0x%08X", (unsigned)hr);
    if (FAILED(hr)) {
        ::MessageBoxW(hwnd, L"无法启动 WebView2。", L"Sonar 配置工具", MB_ICONERROR);
        return 1;
    }
    LogLine("enter message loop");

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    g_web.Reset();
    g_ctrl.Reset();
    if (g_gdiToken) Gdiplus::GdiplusShutdown(g_gdiToken);
    ::CoUninitialize();
    return 0;
}
