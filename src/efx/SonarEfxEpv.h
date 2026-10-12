// ===========================================================================
//  SonarEfxEpv.h —— .epv3 解析 + 用游戏目录里**实际的表**建索引
//
//  为什么需要：随包的 WseEfxIndex.txt 是参考项目 vfx-forge 对**原版**表的快照。
//  玩家改过 epv（换特效路径、加组/记录）后，真实表里的 (组,记录) 会多出来 ——
//  实测 hm_wp11：快照 73 条 vs 磁盘真实 86 条，差的 13 条会被当成"非武器特效"丢弃，
//  表现就是"有的特效完全捕获不到"。
//
//  所以：启动/重载时扫 <游戏目录>\nativePC 下的 hm_wp*.epv3，解析出真实的
//  (组 → 记录) 集合，**覆盖**快照里同名表；磁盘上没有的表继续用快照。
//
//  .epv3 格式（来自 vfx-forge docs/index.md，已用 trailCount 校验）：
//    偏移 0：头部 8 字节；偏移 8：u32 组数；从 0x0C 起按组排列：
//      每组 = { u32 记录数, i16 组号 }
//      记录 = 4 个 NUL 结尾的路径槽 + 338 字节固定体
//              固定体 +8   = recordID (u16)
//              固定体 +114 = boneID (i32)
//    记录区之后：padding + u32 trailCount（== 记录总数，用作校验）
// ===========================================================================
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace sonarefx {

struct EpvRec {
    int         group = 0;
    int         rec = 0;      // 组内下标（调用时传的就是它）
    int         rid = -1;     // 固定体里的 recordID（显示用）
    int         bone = -1;
    std::string path;         // 第一个路径槽（可读名）
};

struct EpvTable {
    std::string name;         // 表名（文件名去掉 .epv3）：hm_wp11 / hm_wp09_01
    std::string file;         // 完整路径（日志里只打文件名）
    std::vector<EpvRec> recs;
};

// 解析单个 .epv3（返回 false 时 why 写明原因）
bool ParseEpv3File(const std::wstring& path, EpvTable& out, std::string& why);

// 递归找 nativePC 下的 hm_wp*.epv3（只进名字是 epv 的目录，避免全盘扫描太慢）
std::vector<std::wstring> FindEpvFiles(const std::wstring& gameRoot, std::size_t maxFiles = 512);

} // namespace sonarefx
