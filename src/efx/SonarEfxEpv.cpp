// ===========================================================================
//  SonarEfxEpv.cpp —— .epv3 解析实现（见 SonarEfxEpv.h）
// ===========================================================================
#include "SonarEfxEpv.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace sonarefx {
namespace {

std::string BaseNameOf(const std::string& p) {
    const std::size_t s = p.find_last_of("\\/");
    return (s == std::string::npos) ? p : p.substr(s + 1);
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 小端读取（文件是不可信输入，逐个做边界检查）
inline bool RdU32(const std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t& v) {
    if (off + 4 > b.size()) return false;
    v = (std::uint32_t)b[off] | ((std::uint32_t)b[off + 1] << 8) |
        ((std::uint32_t)b[off + 2] << 16) | ((std::uint32_t)b[off + 3] << 24);
    return true;
}
inline bool RdU16(const std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t& v) {
    if (off + 2 > b.size()) return false;
    v = (std::uint16_t)((std::uint16_t)b[off] | ((std::uint16_t)b[off + 1] << 8));
    return true;
}
inline bool RdI32(const std::vector<std::uint8_t>& b, std::size_t off, std::int32_t& v) {
    std::uint32_t u = 0;
    if (!RdU32(b, off, u)) return false;
    v = (std::int32_t)u;
    return true;
}

bool ReadWholeFile(const std::wstring& path, std::vector<std::uint8_t>& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64 << 20)) { ::CloseHandle(h); return false; }
    out.resize((std::size_t)sz.QuadPart);
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    ::CloseHandle(h);
    if (!ok || got != out.size()) return false;
    return true;
}

} // namespace

bool ParseEpv3File(const std::wstring& path, EpvTable& out, std::string& why) {
    std::vector<std::uint8_t> b;
    if (!ReadWholeFile(path, b)) { why = "读取失败"; return false; }
    if (b.size() < 16) { why = "文件太小"; return false; }

    std::uint32_t groupCount = 0;
    if (!RdU32(b, 8, groupCount)) { why = "读不到组数"; return false; }
    if (groupCount == 0 || groupCount > 4096) { why = "组数不合理"; return false; }

    out.name = BaseNameOf(WideToUtf8(path));
    if (out.name.size() > 5 && out.name.compare(out.name.size() - 5, 5, ".epv3") == 0)
        out.name = out.name.substr(0, out.name.size() - 5);
    out.file = BaseNameOf(WideToUtf8(path));
    out.recs.clear();

    std::size_t p = 0x0C;
    for (std::uint32_t g = 0; g < groupCount; ++g) {
        std::uint32_t cnt = 0;
        std::uint16_t gn = 0;
        if (!RdU32(b, p, cnt) || !RdU16(b, p + 4, gn)) { why = "组头越界"; return false; }
        p += 6;
        if (cnt > 4096) { why = "记录数不合理"; return false; }
        for (std::uint32_t r = 0; r < cnt; ++r) {
            std::string first;
            for (int k = 0; k < 4; ++k) {
                std::size_t e = p;
                while (e < b.size() && b[e] != 0) ++e;
                if (e >= b.size()) { why = "路径槽没结束符"; return false; }
                if (k == 0 && e > p) first.assign((const char*)&b[p], e - p);
                p = e + 1;
            }
            if (p + 338 > b.size()) { why = "固定体越界"; return false; }
            EpvRec rec;
            rec.group = (int)(std::int16_t)gn;
            rec.rec = (int)r;
            std::uint16_t rid = 0;
            std::int32_t bone = -1;
            if (RdU16(b, p + 8, rid)) rec.rid = (int)rid;
            if (RdI32(b, p + 114, bone)) rec.bone = (int)bone;
            rec.path = first;
            p += 338;
            out.recs.push_back(std::move(rec));
        }
    }

    // 校验：记录区之后应能找到 trailCount == 记录总数（vfx-forge 用的判据）
    bool verified = false;
    const std::size_t lim = (p + 64 < b.size()) ? p + 64 : b.size();
    for (std::size_t i = p; i + 4 <= lim; ++i) {
        std::uint32_t v = 0;
        if (RdU32(b, i, v) && v == out.recs.size()) { verified = true; break; }
    }
    if (!verified) {
        char buf[128];
        _snprintf_s(buf, _TRUNCATE, "校验失败（记录 %zu 条，找不到 trailCount）", out.recs.size());
        why = buf;
        return false;
    }
    if (out.recs.empty()) { why = "没有记录"; return false; }
    return true;
}

std::vector<std::wstring> FindEpvFiles(const std::wstring& gameRoot, std::size_t maxFiles) {
    std::vector<std::wstring> out;
    if (gameRoot.empty()) return out;
    std::wstring root = gameRoot;
    if (!root.empty() && root.back() != L'\\' && root.back() != L'/') root += L'\\';
    root += L"nativePC";

    // 广度优先，只进名字是 epv 的目录（nativePC 下可能有很多 mod，全扫太慢）
    std::vector<std::wstring> dirs{ root };
    for (std::size_t di = 0; di < dirs.size() && out.size() < maxFiles; ++di) {
        const std::wstring dir = dirs[di];
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (dirs.size() < 20000) dirs.push_back(full);
                continue;
            }
            // 只收 hm_wp*.epv3，且所在目录名必须是 epv
            if (name.size() > 10 && name.compare(0, 5, L"hm_wp") == 0) {
                const std::size_t dot = name.find_last_of(L'.');
                if (dot != std::wstring::npos && name.compare(dot, std::wstring::npos, L".epv3") == 0) {
                    // 目录名是 epv？（跳过"新建文件夹"这类临时目录）
                    const std::size_t sl = full.find_last_of(L"\\/");
                    const std::size_t sl2 = (sl == std::wstring::npos) ? std::wstring::npos
                                                                      : full.find_last_of(L"\\/", sl - 1);
                    const std::wstring dirName = (sl2 == std::wstring::npos) ? std::wstring()
                        : full.substr(sl2 + 1, sl - sl2 - 1);
                    if (dirName == L"epv") out.push_back(full);
                }
            }
        } while (::FindNextFileW(h, &fd) && out.size() < maxFiles);
        ::FindClose(h);
    }
    return out;
}

} // namespace sonarefx
