// ===========================================================================
//  bankmod.cpp —— nbnk 解析 / wem 替换导出 / 音频转 wem
//  只依赖 ffmpeg.exe + WwiseConsole.exe（都在本机可定位），不联网。
// ===========================================================================
#include "bankmod.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>

namespace bankmod {
namespace {

uint32_t U32(const std::vector<uint8_t>& b, size_t p) {
    if (p + 4 > b.size()) return 0;
    uint32_t v;
    std::memcpy(&v, &b[p], 4);
    return v;
}
void PutU32(std::vector<uint8_t>& b, size_t p, uint32_t v) {
    if (p + 4 > b.size()) return;
    std::memcpy(&b[p], &v, 4);
}

std::string Tag(const std::vector<uint8_t>& b, size_t p) {
    if (p + 4 > b.size()) return std::string();
    return std::string((const char*)&b[p], 4);
}

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (1LL << 30)) {
        ::CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0, total = 0;
    while (total < out.size()) {
        if (!::ReadFile(h, out.data() + total, (DWORD)(out.size() - total), &rd, nullptr) || rd == 0) break;
        total += rd;
    }
    ::CloseHandle(h);
    out.resize(total);
    return total > 0;
}

bool WriteWhole(const std::wstring& path, const std::vector<uint8_t>& data, std::string& err) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "无法写入文件（路径或权限问题）";
        return false;
    }
    DWORD wr = 0, total = 0;
    while (total < data.size()) {
        if (!::WriteFile(h, data.data() + total, (DWORD)(data.size() - total), &wr, nullptr) || wr == 0) break;
        total += wr;
    }
    ::CloseHandle(h);
    if (total != data.size()) { err = "写入不完整"; return false; }
    return true;
}

std::string BaseNameUtf8(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    std::wstring nm = (s == std::wstring::npos) ? path : path.substr(s + 1);
    size_t d = nm.find_last_of(L'.');
    if (d != std::wstring::npos) nm = nm.substr(0, d);
    // 文件名通常是 ASCII（bank 名），直接转
    std::string out;
    for (wchar_t c : nm) out += (c < 128) ? (char)c : '?';
    return out;
}

std::wstring DirOf(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : path.substr(0, s + 1);
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 读日志尾部（WwiseConsole 的输出可能很大，且可能是 UTF-16）：给用户看"到底哪一步错了"。
//   压成单行、去掉空行，最多 keep 个字符。
std::string ReadTailUtf8(const std::wstring& path, std::size_t keep) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER sz{};
    std::string raw;
    if (::GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
        const long long want = (std::min)((long long)(1LL << 20), sz.QuadPart);
        raw.resize((std::size_t)want);
        ::SetFilePointer(h, (LONG)(sz.QuadPart - want), nullptr, FILE_BEGIN);
        DWORD rd = 0;
        ::ReadFile(h, &raw[0], (DWORD)want, &rd, nullptr);
        raw.resize(rd);
    }
    ::CloseHandle(h);
    std::wstring w;
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        w.assign((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
    } else if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
        const std::size_t n = (raw.size() - 2) / 2;
        w.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            w.push_back((wchar_t)(((unsigned char)raw[2 + i * 2] << 8) | (unsigned char)raw[3 + i * 2]));
    } else {
        const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            raw.c_str(), (int)raw.size(), nullptr, 0);
        if (n > 0) {
            w.resize(n);
            ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.c_str(), (int)raw.size(), &w[0], n);
        } else {
            const int m = ::MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
            if (m > 0) { w.resize(m); ::MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), &w[0], m); }
        }
    }
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) {
        if (c == L'\r' || c == L'\n' || c == L'\t') out.push_back(' ');
        else if (c < 0x20) continue;
        else out += ToUtf8(std::wstring(1, c));
    }
    // 压掉连续空格
    std::string squeezed;
    squeezed.reserve(out.size());
    bool sp = false;
    for (char c : out) {
        if (c == ' ') { if (!sp) squeezed.push_back(c); sp = true; }
        else { squeezed.push_back(c); sp = false; }
    }
    while (!squeezed.empty() && squeezed.back() == ' ') squeezed.pop_back();
    if (squeezed.size() > keep) squeezed = "…" + squeezed.substr(squeezed.size() - keep);
    return squeezed;
}

// ---------------------------------------------------------------------------
//  工程版本适配
//  .wproj 根节点带 SchemaVersion/WwiseVersion/WwiseBuild，这几个字段和生成它的
//  Wwise 版本绑定：Wwise 只会把旧工程升级上去，绝不降级 —— 用 2022 的
//  WwiseConsole 打开 2023 的工程必然失败（一句 "newer version" 就退出）。
//  所以只要 console 的主次版本和工程不一致，就用**用户自己的 console** 现场
//  create-new-project 生成一份同版本工程（放 %TEMP% 的纯 ASCII 目录，生成一次
//  复用），再拿它去 convert-external-source。
// ---------------------------------------------------------------------------
std::string FileVersionUtf8(const std::wstring& exe) {
    DWORD dummy = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (!size) return std::string();
    std::vector<unsigned char> buf(size);
    if (!::GetFileVersionInfoW(exe.c_str(), 0, size, buf.data())) return std::string();
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!::VerQueryValueW(buf.data(), L"\\", (LPVOID*)&fi, &len) || !fi ||
        len < sizeof(VS_FIXEDFILEINFO)) return std::string();
    char out[64] = {};
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u",
                  (unsigned)(fi->dwFileVersionMS >> 16), (unsigned)(fi->dwFileVersionMS & 0xFFFF),
                  (unsigned)(fi->dwFileVersionLS >> 16), (unsigned)(fi->dwFileVersionLS & 0xFFFF));
    return std::string(out);
}

// 从工程文件里取 WwiseVersion="v2023.1.13" → "2023.1.13"
std::string WprojVersion(const std::wstring& proj) {
    std::vector<uint8_t> head;
    HANDLE h = ::CreateFileW(proj.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    char buf[2048] = {};
    DWORD rd = 0;
    ::ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    ::CloseHandle(h);
    const std::string s(buf, buf + rd);
    const std::string key = "WwiseVersion=\"v";
    const size_t at = s.find(key);
    if (at == std::string::npos) return std::string();
    const size_t from = at + key.size();
    size_t to = s.find('"', from);
    if (to == std::string::npos) return std::string();
    return s.substr(from, to - from);
}

// "2022.1.6.8215" → "2022.1"
std::string MajorMinorOf(const std::string& v) {
    const size_t a = v.find('.');
    if (a == std::string::npos) return v;
    const size_t b = v.find('.', a + 1);
    return (b == std::string::npos) ? v : v.substr(0, b);
}

// 用指定 console 生成/复用一个同版本工程；失败返回空。
std::wstring EnsureProjectFor(const std::wstring& console, const std::string& ver) {
    std::string mm = MajorMinorOf(ver);
    if (mm.empty()) return std::wstring();
    for (char& c : mm) if (c == '.' || (c >= '0' && c <= '9')) {} else c = '_';
    wchar_t tmpPath[MAX_PATH] = {};
    if (!::GetTempPathW(MAX_PATH, tmpPath)) return std::wstring();
    std::wstring base(tmpPath);
    if (!base.empty() && base.back() == L'\\') base.pop_back();
    // create-new-project 要求 <目录名>\<同名>.wproj，并且目录必须由它自己建
    const std::wstring name = L"sonar_wwproj_" + std::wstring(mm.begin(), mm.end());
    const std::wstring dir = base + L"\\" + name;
    const std::wstring proj = dir + L"\\" + name + L".wproj";
    if (::GetFileAttributesW(proj.c_str()) != INVALID_FILE_ATTRIBUTES) return proj;

    const std::wstring cmd = L"\"" + console + L"\" create-new-project \"" + proj +
                             L"\" --platform Windows --quiet";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi)) return std::wstring();
    ::WaitForSingleObject(pi.hProcess, 120000);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    if (::GetFileAttributesW(proj.c_str()) == INVALID_FILE_ATTRIBUTES) return std::wstring();
    return proj;
}

bool LoadPackage(BankFile& out, std::string& err) {    const std::vector<uint8_t>& b = out.bytes;
    if (b.size() < 32) { err = "npck 文件太小（不足一个文件包的头部）"; return false; }
    if (Tag(b, 0) != "AKPK") { err = "不是 AKPK 文件包（开头不是 AKPK）"; return false; }
    const uint32_t headerLen = U32(b, 4);
    const uint32_t unkn2 = U32(b, 8);
    const uint32_t langLen = U32(b, 12);
    const uint32_t bnkLen = U32(b, 16);
    const uint32_t wemTableLen = U32(b, 20);
    const uint32_t unknLen = U32(b, 24);
    // 头里这五个长度加起来必须和文件对得上，否则说明不是这个版本
    if ((uint64_t)28 + langLen + bnkLen + wemTableLen + unknLen > b.size()) {
        err = "npck 头部长度越界（文件损坏或不是这个版本的文件包）";
        return false;
    }
    const size_t langPos = 28;
    const size_t bnkPos = langPos + langLen;
    if (bnkLen < 4 || wemTableLen < 4) { err = "npck 头部异常（缺少 bnk / wem 表）"; return false; }
    if (U32(b, bnkPos + bnkLen) > b.size()) { err = "npck 的 wem 数量越界"; return false; }
    const uint32_t count = U32(b, bnkPos + bnkLen);
    const size_t recPos = bnkPos + bnkLen + 4;
    if (recPos + (size_t)count * 20 > b.size()) { err = "npck 的 wem 表越界"; return false; }
    if (recPos + (size_t)count * 20 > 8 + (size_t)headerLen) { err = "npck 的 wem 表与头部长度不一致"; return false; }
    for (uint32_t i = 0; i < count; ++i) {
        const size_t p = recPos + (size_t)i * 20;
        MediaEntry e;
        e.id = U32(b, p);
        e.size = U32(b, p + 8);
        e.offset = U32(b, p + 12);          // 绝对偏移
        e.language = U32(b, p + 16);
        e.seq = (int)i + 1;
        if ((uint64_t)e.offset + e.size > b.size()) {
            err = "npck 里第 " + std::to_string(i + 1) + " 个 wem 的位置越界（文件损坏）";
            return false;
        }
        out.media.push_back(e);
    }
    out.isPackage = true;
    out.pkgUnkn2 = unkn2;
    out.pkgLangTable.assign(b.begin() + langPos, b.begin() + bnkPos);
    out.pkgBnkTable.assign(b.begin() + bnkPos, b.begin() + bnkPos + bnkLen);
    {

        const size_t tailPos = recPos + (size_t)count * 20;
        const size_t tailEnd = std::min<size_t>(8 + (size_t)headerLen, b.size());
        out.pkgTail.assign(b.begin() + tailPos, b.begin() + (std::max)(tailPos, tailEnd));
    }
    if (out.media.empty()) { err = "这个 npck 里没有 wem"; return false; }
    return true;
}

}  // namespace

std::wstring FindWwiseConsole() {
    // 1) 环境变量 WWISEROOT（Wwise Launcher 安装后会设置；但它是"给新进程"的，
    //    装完 Wwise 必须**重启配置工具**才能读到）
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = ::GetEnvironmentVariableW(L"WWISEROOT", buf, MAX_PATH * 2);
    if (n > 0) {
        std::wstring p = std::wstring(buf) + L"\\Authoring\\x64\\Release\\bin\\WwiseConsole.exe";
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    // 2) 常见位置兜底：**扫描所有固定磁盘**（以前只扫 C/D/E，装在别的盘就"检测不到"）
    std::vector<std::wstring> roots;
    {
        wchar_t drives[512] = {};
        if (::GetLogicalDriveStringsW(511, drives) > 0) {
            for (const wchar_t* d = drives; *d; d += wcslen(d) + 1) {
                if (::GetDriveTypeW(d) != DRIVE_FIXED) continue;
                const std::wstring dr(d);   // 形如 "F:\"
                const wchar_t* subs[] = { L"Audiokinetic", L"Program Files\\Audiokinetic",
                                          L"Program Files (x86)\\Audiokinetic", L"Wwise" };
                for (const wchar_t* s : subs) roots.push_back(dr + s);
            }
        }
    }
    for (const std::wstring& root : roots) {
        std::wstring dirPat = root + L"\\*";
        WIN32_FIND_DATAW dd{};
        HANDLE dh = ::FindFirstFileW(dirPat.c_str(), &dd);
        if (dh == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (dd.cFileName[0] == L'.') continue;
            // <root>\<版本>\Authoring\x64\Release\bin\WwiseConsole.exe
            const std::wstring p = root + L"\\" + dd.cFileName +
                                   L"\\Authoring\\x64\\Release\\bin\\WwiseConsole.exe";
            if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ::FindClose(dh);
                return p;
            }
        } while (::FindNextFileW(dh, &dd));
        ::FindClose(dh);
    }
    return std::wstring();
}

bool LoadBank(const std::wstring& path, BankFile& out, std::string& err) {
    out = BankFile();
    out.path = path;
    out.name = BaseNameUtf8(path);
    if (!ReadWhole(path, out.bytes)) {
        err = "读不到文件（可能正被占用、或大于 1GB）";
        return false;
    }

    // .npck = Wwise File Package（AKPK），和 nbnk（SoundBank）是两种完全不同的容器：
    //  nbnk 里是 BKHD/DIDX/DATA/HIRC 段；npck 里是一张 wem 表 + 一堆裸 wem 数据。
    if (Tag(out.bytes, 0) == "AKPK") return LoadPackage(out, err);

    size_t p = 0;
    const size_t n = out.bytes.size();
    while (p + 8 <= n) {
        const std::string tag = Tag(out.bytes, p);
        const uint32_t len = U32(out.bytes, p + 4);
        if (p + 8 + len > n) { err = "段越界（文件损坏或不是 nbnk）"; return false; }
        if (tag == "BKHD") { out.bkhdPos = p; out.bkhdLen = len; }
        else if (tag == "DIDX") {
            out.didxPos = p; out.didxLen = len;
            const size_t cnt = len / 12;
            for (size_t i = 0; i < cnt; ++i) {
                MediaEntry e;
                e.id = U32(out.bytes, p + 8 + i * 12);
                e.offset = U32(out.bytes, p + 8 + i * 12 + 4);
                e.size = U32(out.bytes, p + 8 + i * 12 + 8);
                e.seq = (int)i + 1;
                out.media.push_back(e);
            }
        } else if (tag == "DATA") { out.dataPos = p; out.dataLen = len; }
        else if (tag == "HIRC") { out.hircPos = p; out.hircLen = len; }
        else out.otherChunks.push_back(std::make_pair(tag, p));
        p += 8 + len;
    }
    if (out.media.empty() || out.dataPos == 0) {
        err = "不是有效的 nbnk（缺 DIDX/DATA），也不是 npck（开头不是 AKPK）";
        return false;
    }
    return true;
}

bool ExtractWem(const BankFile& bank, uint32_t mediaId, std::vector<uint8_t>& wem) {
    for (const MediaEntry& e : bank.media) {
        if (e.id != mediaId) continue;
        const size_t abs = bank.isPackage ? (size_t)e.offset : (bank.dataPos + 8 + e.offset);
        if (abs + e.size > bank.bytes.size()) return false;
        wem.assign(bank.bytes.begin() + abs, bank.bytes.begin() + abs + e.size);
        return true;
    }
    return false;
}

// npck 导出：头部其余字节原样保留，只按新的 wem 字节数重算 wem 表里的 offset/size，
// 再把所有 wem 顺序写回。布局沿用 AKPK：头 8 字节 + headerLength，数据紧跟其后。
static bool ExportPackage(const BankFile& bank,
                          const std::vector<std::pair<uint32_t, std::wstring>>& replacements,
                          const std::wstring& outPath, std::string& err) {
    struct Item { uint32_t id; uint32_t lang; std::vector<uint8_t> data; };
    std::vector<Item> items;
    items.reserve(bank.media.size());
    for (const MediaEntry& e : bank.media) {
        Item it;
        it.id = e.id;
        it.lang = e.language;
        const size_t abs = (size_t)e.offset;
        if (abs + e.size > bank.bytes.size()) { err = "原 npck 的 wem 数据越界"; return false; }
        it.data.assign(bank.bytes.begin() + abs, bank.bytes.begin() + abs + e.size);
        for (const auto& r : replacements) {
            if (r.first != e.id) continue;
            std::vector<uint8_t> rep;
            if (!ReadWhole(r.second, rep) || rep.size() < 12) {
                err = "替换用的 wem 读不到或过小：" + ToUtf8(r.second);
                return false;
            }
            it.data.swap(rep);
        }
        items.push_back(std::move(it));
    }

    const uint32_t langLen = (uint32_t)bank.pkgLangTable.size();
    const uint32_t bnkLen = (uint32_t)bank.pkgBnkTable.size();
    const uint32_t unknLen = (uint32_t)bank.pkgTail.size();
    const uint32_t count = (uint32_t)items.size();
    const uint32_t wemTableLen = count * 20 + 4;
    const uint32_t headerLen = 20 + langLen + bnkLen + wemTableLen + unknLen;
    // 实测游戏里的 npck：每个 wem 的 offset 都是 16 字节对齐的（数据区起点也一样），
    //  中间的空隙补 0。不按这个规则重排，导出的包虽然"能解析"，但和原文件不一致。
    const size_t dataStart = ((size_t)8 + headerLen + 15) & ~(size_t)15;

    std::vector<uint8_t> out;
    out.resize(dataStart, 0);
    std::memcpy(&out[0], "AKPK", 4);
    PutU32(out, 4, headerLen);
    PutU32(out, 8, bank.pkgUnkn2);
    PutU32(out, 12, langLen);
    PutU32(out, 16, bnkLen);
    PutU32(out, 20, wemTableLen);
    PutU32(out, 24, unknLen);
    size_t p = 28;
    if (langLen) { std::memcpy(&out[p], bank.pkgLangTable.data(), langLen); p += langLen; }
    if (bnkLen) { std::memcpy(&out[p], bank.pkgBnkTable.data(), bnkLen); p += bnkLen; }
    PutU32(out, p, count);
    p += 4;
    for (size_t i = 0; i < items.size(); ++i) {
        PutU32(out, p + 0, items[i].id);
        PutU32(out, p + 4, 1);                                  // 原文件这里恒为 1
        PutU32(out, p + 8, (uint32_t)items[i].data.size());
        PutU32(out, p + 12, 0);                                 // offset：数据区拼好后统一回填
        PutU32(out, p + 16, items[i].lang);
        p += 20;
    }
    if (unknLen) std::memcpy(&out[p], bank.pkgTail.data(), unknLen);

    // 数据区：每条 16 字节对齐、顺序紧接
    for (size_t i = 0; i < items.size(); ++i) {
        while (out.size() % 16 != 0) out.push_back(0);
        PutU32(out, 28 + langLen + bnkLen + 4 + i * 20 + 12, (uint32_t)out.size());
        out.insert(out.end(), items[i].data.begin(), items[i].data.end());
    }
    return WriteWhole(outPath, out, err);
}

bool ExportBank(const BankFile& bank,
                const std::vector<std::pair<uint32_t, std::wstring>>& replacements,
                const std::wstring& outPath, std::string& err) {
    if (bank.isPackage) return ExportPackage(bank, replacements, outPath, err);
    // 1) 组装新的 wem 数据列表（原顺序；被替换的换字节）
    struct Item { uint32_t id; std::vector<uint8_t> data; };
    std::vector<Item> items;
    items.reserve(bank.media.size());
    for (const MediaEntry& e : bank.media) {
        Item it;
        it.id = e.id;
        const size_t abs = bank.dataPos + 8 + e.offset;
        if (abs + e.size > bank.bytes.size()) { err = "原 bank 的 wem 数据越界"; return false; }
        it.data.assign(bank.bytes.begin() + abs, bank.bytes.begin() + abs + e.size);
        for (const auto& r : replacements) {
            if (r.first != e.id) continue;
            std::vector<uint8_t> rep;
            if (!ReadWhole(r.second, rep) || rep.size() < 12) {
                err = "替换用的 wem 读不到或过小：" + ToUtf8(r.second);
                return false;
            }
            it.data.swap(rep);
        }
        items.push_back(std::move(it));
    }

    // 2) 重建 DATA 与 DIDX（实测 nbnk 里各 wem 是**紧接**的、不做 16 字节对齐）
    std::vector<uint8_t> data;
    std::vector<uint8_t> didx;
    didx.resize(items.size() * 12);
    for (size_t i = 0; i < items.size(); ++i) {
        const uint32_t off = (uint32_t)data.size();
        data.insert(data.end(), items[i].data.begin(), items[i].data.end());
        PutU32(didx, i * 12 + 0, items[i].id);
        PutU32(didx, i * 12 + 4, off);
        PutU32(didx, i * 12 + 8, (uint32_t)items[i].data.size());
    }

    // 3) 拼文件：BKHD -> DIDX -> DATA -> HIRC -> 其它（顺序与原 bank 一致，缺失的跳过）
    std::vector<uint8_t> outBytes;
    outBytes.reserve(bank.bytes.size() + data.size());
    auto appendChunk = [&](const char* tag, const std::vector<uint8_t>& payload) {
        const size_t at = outBytes.size();
        outBytes.resize(at + 8 + payload.size());
        std::memcpy(&outBytes[at], tag, 4);
        PutU32(outBytes, at + 4, (uint32_t)payload.size());
        if (!payload.empty()) std::memcpy(&outBytes[at + 8], payload.data(), payload.size());
    };
    auto origPayload = [&](size_t pos, size_t len) {
        return std::vector<uint8_t>(bank.bytes.begin() + pos + 8, bank.bytes.begin() + pos + 8 + len);
    };

    if (bank.bkhdLen) appendChunk("BKHD", origPayload(bank.bkhdPos, bank.bkhdLen));
    appendChunk("DIDX", didx);
    appendChunk("DATA", data);
    if (bank.hircLen) appendChunk("HIRC", origPayload(bank.hircPos, bank.hircLen));
    for (const auto& oc : bank.otherChunks) {
        const uint32_t len = U32(bank.bytes, oc.second + 4);
        appendChunk(oc.first.c_str(), origPayload(oc.second, len));
    }

    return WriteWhole(outPath, outBytes, err);
}

bool ConvertToWem(const std::vector<std::wstring>& audioFiles,
                  const std::wstring& wwiseProj,
                  const std::wstring& ffmpegExe,
                  const std::wstring& workDir,
                  std::vector<std::wstring>& outWems,
                  std::string& log,
                  const std::wstring& wwiseConsoleExe) {
    outWems.clear();
    if (audioFiles.empty()) { log = "没有音频文件"; return false; }
    if (!::CreateDirectoryW(workDir.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
        log = "无法创建临时目录";
        return false;
    }
    const std::wstring wavDir = workDir + L"\\wav\\";
    ::CreateDirectoryW(wavDir.c_str(), nullptr);

    // 1) ffmpeg：任意格式 -> wav（48kHz / 立体声，和游戏内 wem 一致的常见配置）
    std::vector<std::wstring> wavs;
    for (const std::wstring& src : audioFiles) {
        std::wstring base = BaseNameUtf8(src).empty() ? L"a" : std::wstring();
        // base 用宽字符版
        size_t s = src.find_last_of(L"\\/");
        std::wstring nm = (s == std::wstring::npos) ? src : src.substr(s + 1);
        size_t d = nm.find_last_of(L'.');
        if (d != std::wstring::npos) nm = nm.substr(0, d);
        if (nm.empty()) nm = L"a";
        (void)base;
        const std::wstring dst = wavDir + nm + L".wav";
        // 已是 wav 就跳过转码（省时间、避免二次重采样）
        size_t ext = src.find_last_of(L'.');
        const bool isWav = (ext != std::wstring::npos &&
                            (src.substr(ext) == L".wav" || src.substr(ext) == L".WAV"));
        if (isWav) {
            if (!::CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
                log = "复制 wav 失败：" + ToUtf8(src);
                return false;
            }
        } else {
            if (ffmpegExe.empty() || ::GetFileAttributesW(ffmpegExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
                log = "需要 ffmpeg.exe 才能转换 " + ToUtf8(src) +
                      "（点「音效包制作」页里的「选择 ffmpeg.exe…」指定它的位置；"
                      "或把 ffmpeg.exe 放到 wemkit\\ 目录下）";
                return false;
            }
            std::wstring cmd = L"\"" + ffmpegExe + L"\" -hide_banner -loglevel error -y -i \"" + src +
                               L"\" -ar 48000 -ac 2 \"" + dst + L"\"";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(0);
            if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                log = "ffmpeg 启动失败";
                return false;
            }
            ::WaitForSingleObject(pi.hProcess, 60000);
            DWORD code = 1;
            ::GetExitCodeProcess(pi.hProcess, &code);
            ::CloseHandle(pi.hProcess);
            ::CloseHandle(pi.hThread);
            if (code != 0 || ::GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) {
                log = "ffmpeg 转换失败：" + ToUtf8(src);
                return false;
            }
        }
        wavs.push_back(dst);
    }

    // 2) 生成 .wsources 清单（Wwise 的外部源列表格式）
    const std::wstring listPath = workDir + L"\\list.wsources";
    {
        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        xml += "<ExternalSourcesList SchemaVersion=\"1\" Root=\"" + ToUtf8(wavDir) + "\">\n";
        for (const std::wstring& w : wavs) {
            size_t s = w.find_last_of(L"\\/");
            std::wstring leaf = (s == std::wstring::npos) ? w : w.substr(s + 1);
            xml += "\t<Source Path=\"" + ToUtf8(leaf) + "\" Conversion=\"Vorbis Quality High\"/>\n";
        }
        xml += "</ExternalSourcesList>\n";
        HANDLE h = ::CreateFileW(listPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { log = "写 list.wsources 失败"; return false; }
        DWORD wr = 0;
        ::WriteFile(h, xml.data(), (DWORD)xml.size(), &wr, nullptr);
        ::CloseHandle(h);
    }

    // 3) WwiseConsole 转换（输出到 <workDir>\out\Windows\*.wem）
    const std::wstring outDir = workDir + L"\\out";
    ::CreateDirectoryW(outDir.c_str(), nullptr);
    const std::wstring winDir = outDir + L"\\Windows\\";
    // ★ 先清掉上次留下的 wem：否则这次转换失败时，我们会把旧文件当成"成功"
    {
        WIN32_FIND_DATAW ofd{};
        HANDLE oh = ::FindFirstFileW((winDir + L"*.wem").c_str(), &ofd);
        if (oh != INVALID_HANDLE_VALUE) {
            do { ::DeleteFileW((winDir + ofd.cFileName).c_str()); } while (::FindNextFileW(oh, &ofd));
            ::FindClose(oh);
        }
    }
    // 用户手动指定的优先（Wwise 装在别的盘/改过目录名），指定失效再退回自动查找
    std::wstring console = wwiseConsoleExe;
    if (!console.empty() && ::GetFileAttributesW(console.c_str()) == INVALID_FILE_ATTRIBUTES)
        console.clear();
    if (console.empty()) console = FindWwiseConsole();
    if (console.empty()) { log = "找不到 WwiseConsole.exe（需要装 Wwise，或设 WWISEROOT 环境变量）"; return false; }
    // 工程版本不一致时现场生成一份同版本工程（否则 2022 的 console 打不开 2023 的工程）
    std::wstring proj = wwiseProj;
    {
        const std::string cver = FileVersionUtf8(console);
        const std::string pver = WprojVersion(wwiseProj);
        if (!cver.empty() && !pver.empty() && MajorMinorOf(cver) != MajorMinorOf(pver)) {
            const std::wstring alt = EnsureProjectFor(console, cver);
            if (!alt.empty()) {
                proj = alt;
            } else {
                log += "（工程是 " + pver + "，而这个 WwiseConsole 是 " + cver +
                       "：自动生成同版本工程失败）";
            }
        }
    }
    // 输出到日志文件（不再用 --quiet）：失败时把日志尾巴贴给用户，否则只剩一句"没有产出 wem"
    const std::wstring logPath = workDir + L"\\wwise.log";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = ::CreateFileW(logPath.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring full = L"\"" + console + L"\" convert-external-source \"" + proj +
                        L"\" --source-file \"" + listPath + L"\" --output \"" + outDir + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (hLog != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hLog;
        si.hStdError = hLog;
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(full.begin(), full.end());
    buf.push_back(0);
    if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
                          (hLog != INVALID_HANDLE_VALUE) ? TRUE : FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (hLog != INVALID_HANDLE_VALUE) ::CloseHandle(hLog);
        log = "WwiseConsole 启动失败";
        return false;
    }
    ::WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    if (hLog != INVALID_HANDLE_VALUE) ::CloseHandle(hLog);

    // 4) 收集结果（<outDir>\Windows\*.wem）
    WIN32_FIND_DATAW fd{};
    HANDLE fh = ::FindFirstFileW((winDir + L"*.wem").c_str(), &fd);
    if (fh == INVALID_HANDLE_VALUE) {
        log = "Wwise 没有产出 wem（退出码 " + std::to_string((int)code) + "）";
        // 明确的"装得不全"：完整安装里 Authoring\Data 一定在 bin 往上三层
        {
            std::wstring up = console;
            for (int i = 0; i < 3 && !up.empty(); ++i) {
                std::size_t sl = up.find_last_of(L"\\/");
                if (sl == std::wstring::npos) { up.clear(); break; }
                up.resize(sl);
            }
            if (!up.empty() && ::GetFileAttributesW((up + L"\\Data").c_str()) == INVALID_FILE_ATTRIBUTES)
                log += "。这个 WwiseConsole 看着不是完整安装（缺少同级的 Authoring\\Data）";
        }
        const std::string tail = ReadTailUtf8(logPath, 900);
        if (!tail.empty()) log += "。日志：" + tail;
        return false;
    }
    do {
        outWems.push_back(winDir + fd.cFileName);
    } while (::FindNextFileW(fh, &fd));
    ::FindClose(fh);
    if (outWems.empty()) { log = "Wwise 没有产出 wem"; return false; }
    log = "转换完成，共 " + std::to_string(outWems.size()) + " 个 wem";
    return true;
}

}  // namespace bankmod
