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
    if (!ReadWhole(path, out.bytes)) { err = "读不到 nbnk 文件"; return false; }

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
    if (out.media.empty() || out.dataPos == 0) { err = "不是有效的 nbnk（缺 DIDX/DATA）"; return false; }
    return true;
}

bool ExtractWem(const BankFile& bank, uint32_t mediaId, std::vector<uint8_t>& wem) {
    for (const MediaEntry& e : bank.media) {
        if (e.id != mediaId) continue;
        const size_t abs = bank.dataPos + 8 + e.offset;
        if (abs + e.size > bank.bytes.size()) return false;
        wem.assign(bank.bytes.begin() + abs, bank.bytes.begin() + abs + e.size);
        return true;
    }
    return false;
}

bool ExportBank(const BankFile& bank,
                const std::vector<std::pair<uint32_t, std::wstring>>& replacements,
                const std::wstring& outPath, std::string& err) {
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

    // 2) 重建 DATA（每个 wem 16 字节对齐）与 DIDX
    std::vector<uint8_t> data;
    std::vector<uint8_t> didx;
    didx.resize(items.size() * 12);
    for (size_t i = 0; i < items.size(); ++i) {
        const uint32_t off = (uint32_t)data.size();
        data.insert(data.end(), items[i].data.begin(), items[i].data.end());
        while (data.size() % 16 != 0) data.push_back(0);   // 对齐（和原 bank 一致）
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
                  std::string& log) {
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
                      "（把 ffmpeg.exe 放到 tools\\sound2wem\\ffmpeg\\bin\\，或在设置里指定）";
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
    std::wstring cmd = L"\"" + wwiseProj + L"\"";
    std::wstring console = FindWwiseConsole();
    if (console.empty()) { log = "找不到 WwiseConsole.exe（需要装 Wwise，或设 WWISEROOT 环境变量）"; return false; }
    std::wstring full = L"\"" + console + L"\" convert-external-source \"" + wwiseProj +
                        L"\" --source-file \"" + listPath + L"\" --output \"" + outDir + L"\" --quiet";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(full.begin(), full.end());
    buf.push_back(0);
    if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        log = "WwiseConsole 启动失败";
        return false;
    }
    ::WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    (void)cmd;

    // 4) 收集结果（<outDir>\Windows\*.wem）
    const std::wstring winDir = outDir + L"\\Windows\\";
    WIN32_FIND_DATAW fd{};
    HANDLE fh = ::FindFirstFileW((winDir + L"*.wem").c_str(), &fd);
    if (fh == INVALID_HANDLE_VALUE) {
        log = "Wwise 没有产出 wem（工程缺少 Windows 平台？转换设置？退出码 " +
              std::to_string((int)code) + "）";
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
