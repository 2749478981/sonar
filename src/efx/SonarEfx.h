// ===========================================================================
//  SonarEfx.h —— 特效捕获（实验性）
//
//  目标：抓到"游戏正在生成哪个武器特效"，作为触发音效的标识。
//  标识 = (epv 表名, group, 组内记录下标)   例：hm_wp03 · 组 2005 · #2
//
//  做法
//  ----
//  * 挂钩引擎的特效生成函数 Effect:Ctor(cPlayerEffect*, group, record)。
//    该函数的 RVA/特征码来自参考项目 vfx-forge（docs/index.md）：
//      入口 RVA 0x0AE0D70，特征码 E9 FB C8 FF FF（真身 0x0ADD670）
//  * **磁盘上的 exe 是 DRM 加密的**（实测 .text 全段高熵、入口点在 .bind），
//    所以只能在**运行时扫内存**里的模块，不能静态扫文件。
//  * 抓到 (group, record) 后用随包的 WseEfxIndex.txt 反查它属于哪张 hm_wp* 表；
//    查不到的说明不是武器特效（怪物/环境/UI），直接丢弃 —— 这就是"武器特效过滤"。
//
//  安全性
//  ------
//  * 全程只读游戏内存 + 只 hook 一个函数；不写玩家状态、不改调用参数。
//  * hook 回调里**只往环形缓冲塞 8 字节**（不加锁、不 new、不写盘）。
//    真正的查表与写日志在主循环的 Flush() 里做（见 MHW_DLL_Plugin_Skill.md 的
//    0xC0000005 教训：hook 里做重活会崩）。
//  * 扫不到特征码 / 挂钩失败时：Available()=false，状态写进日志（不静默失败）。
// ===========================================================================
#pragma once

#include <string>

namespace sonarefx {

// 初始化：扫内存定位 Effect:Ctor → MinHook；加载 WseEfxIndex.txt。
// moduleDir：插件目录（含 WseEfxIndex.txt，日志也写这里）
// log：把自检结果追加到调用方的日志字符串
bool Init(const std::wstring& moduleDir, std::string& log);

// 卸载：解除 hook（进程退出前调用；不调用也不会崩，只是 hook 一直挂着）
void Shutdown();

// 是否挂钩成功（GUI/日志用它判断"特效捕获"能不能用）
bool Available();

// 自检结果（一行，给人看的）
const std::string& Status();

// 开关（ini: EfxCapture，默认开）。关掉后 hook 仍在，但不记账、不写日志。
void SetEnabled(bool on);
bool Enabled();

// 诊断开关（ini: EfxLogAll，默认开）：把**所有** hook 到的原始调用（含非武器表）
// 也写进日志，每个 (组,记录) 只写一次（最多 300 个）。
// 用途：区分"hook 根本没被调用"和"被调用了但值不在索引里"。
void SetLogAll(bool on);

// 当前武器类型提供者（-1 = 未知）：用于在多张候选表里挑对的那张
void SetWeaponProvider(int (*fn)());

// 主循环每帧调用：把 hook 记下的事件查表 + 写日志
void Flush();

// 统计（GUI 显示用）
struct Stats {
    unsigned long long calls = 0;    // hook 到的总调用数
    unsigned long long weapon = 0;   // 判定为武器特效（落在 hm_wp* 表里）
    unsigned long long other = 0;    // 非武器特效（丢弃）
    unsigned long long dup = 0;      // 短时间内重复（丢弃）
    unsigned long long logLines = 0; // 实际写入日志的行数
};
Stats GetStats();

} // namespace sonarefx
