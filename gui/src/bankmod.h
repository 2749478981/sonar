// ===========================================================================
//  bankmod.h —— nbnk 音效替换（Mod 制作）核心
//
//  流程（全部本地、不联网）：
//    音频(wav/mp3/ogg/flac) --ffmpeg--> wav --WwiseConsole--> .wem
//    .wem --> 替换原 nbnk 里的 DATA 字节 + 重算 DIDX --> 导出到 nativePC
//
//  nbnk 段结构（Wwise SoundBank v120）：
//    BKHD(头) -> DIDX(媒体索引, 每条 12B: id/offset/size) -> DATA(wem 原始数据) -> HIRC(层级对象)
//  替换只动 DIDX + DATA：HIRC/BKHD 原样保留 → 游戏里的 event→media 映射不变，
//  只是该 media 的声音被换掉（这正是 mod 需要的行为）。
// ===========================================================================
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace bankmod {

struct MediaEntry {
    uint32_t id = 0;        // media id（wem 的唯一标识）
    uint32_t offset = 0;    // nbnk：相对 DATA 起点；npck：文件内绝对偏移
    uint32_t size = 0;      // wem 字节数
    uint32_t language = 0;  // npck 专用（语言枚举）
    int      seq = 0;       // 在 bank 里第几个（1-based）
};

struct BankFile {
    std::wstring path;
    std::string  name;                  // 文件名（不含扩展名）
    std::vector<uint8_t> bytes;
    std::vector<MediaEntry> media;

    // ---- SoundBank（nbnk）----
    // 段位置（用于导出时重建）
    size_t bkhdPos = 0, bkhdLen = 0;
    size_t didxPos = 0, didxLen = 0;
    size_t dataPos = 0, dataLen = 0;
    size_t hircPos = 0, hircLen = 0;
    std::vector<std::pair<std::string, size_t>> otherChunks;   // 未知段：tag + 位置（原样保留）

    // ---- File Package（npck / AKPK）----
    //  结构：AKPK 头 -> 语言表 -> bnk 表 -> wem 表(count + 每条 20B) -> 头尾结构 -> 各 wem 数据
    //  导出时只重建 wem 表里的 offset/size，其余字节原样搬。
    bool isPackage = false;
    uint32_t pkgUnkn2 = 1;
    std::vector<uint8_t> pkgLangTable;   // 语言表原始字节
    std::vector<uint8_t> pkgBnkTable;    // bnk 表原始字节（Capcom 通常是 4 字节 0）
    std::vector<uint8_t> pkgTail;        // wem 表之后、数据区之前的 unknStruct 字节

    // 导出时用的扩展名（nbnk / npck）
    const char* Ext() const { return isPackage ? "npck" : "nbnk"; }
};

// 解析 nbnk（只读，失败时 err 有说明）
bool LoadBank(const std::wstring& path, BankFile& out, std::string& err);

// 取某条 media 的 wem 原始字节（用于试听/校验）
bool ExtractWem(const BankFile& bank, uint32_t mediaId, std::vector<uint8_t>& wem);

// 用给定 wem 替换若干 media，导出新 nbnk。
//   replacements: media id -> wem 文件路径（读其字节替换）
//   输出顺序：BKHD -> DIDX(重建) -> DATA(重建, 16 字节对齐) -> HIRC -> 其它段
bool ExportBank(const BankFile& bank,
                const std::vector<std::pair<uint32_t, std::wstring>>& replacements,
                const std::wstring& outPath, std::string& err);

// 音频 -> wem（ffmpeg 转 wav；WwiseConsole 转 Vorbis wem）。
//   wwiseProj: wavtowemscript.wproj 路径；ffmpegExe: ffmpeg.exe 路径；workDir: 临时目录
//   成功时 outWems 与 audioFiles 一一对应（同名 .wem）
bool ConvertToWem(const std::vector<std::wstring>& audioFiles,
                  const std::wstring& wwiseProj,
                  const std::wstring& ffmpegExe,
                  const std::wstring& workDir,
                  std::vector<std::wstring>& outWems,
                  std::string& log);

// 找 WwiseConsole.exe（优先 WWISEROOT 环境变量）
std::wstring FindWwiseConsole();

}  // namespace bankmod
