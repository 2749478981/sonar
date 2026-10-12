// ===========================================================================
//  efx_selftest.cpp —— SonarEfx（特效捕获）离线自检
//
//  目的：在**不进游戏**的情况下，把整条管线验证掉：
//    内存特征码扫描 → 解析跳板目标 → 判断函数序言 → MinHook 挂钩 →
//    调用时记账（环形缓冲）→ 索引反查（hm_wp* 过滤）→ 写日志 → 统计
//
//  做法：在本 exe 的可执行段里放一段 blob：
//      stub  @ 偏移 0      : 48 83 EC 28 / 48 83 C4 28 / C3   （合法函数序言 + ret）
//      thunk @ 偏移 14080  : E9 FB C8 FF FF                     （rel32 = -14085 = 0x3705）
//  即：thunk 的目标 = 14080 + 5 - 14085 = 0 = stub ✓
//  游戏里同一条跳板：0xAE0D70 + 5 - 14085 = 0xADD670 = 文档记录的真身 ✓
//
//  验收：
//    1) 跳板字节/目标/序言三项自洽
//    2) Init 成功、Available() == true、Status() 含"挂钩成功"
//    3) (2005,2) → 日志出现 table=hm_wp03 group=2005 rec=2；(2001,0) 同理
//    4) (9999,9) 非武器表 → 丢弃；(2005,2) 连续两次 → 去重
//    5) SetEnabled(false) 后不再记账
//    6) 破坏特征码后 Init 失败且原因写清楚（不静默）
// ===========================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>

#include "SonarEfx.h"

// ---- 可执行段里的 blob：stub @0，跳板 @14080 -------------------------------
#pragma section(".efxprobe", read, execute)
__declspec(allocate(".efxprobe")) unsigned char g_blob[14085] = {
    // 偏移 0：stub（sub rsp,0x28 / add rsp,0x28 / ret）—— 合法函数序言
    0x48, 0x83, 0xEC, 0x28,
    0x48, 0x83, 0xC4, 0x28,
    0xC3,
    // 其余为 0；跳板字节在运行时写入偏移 14080
};

static const unsigned kThunkOff = 14080;   // 14080 + 5 - 14085 = 0 → 目标 = stub

using FnEffectCtor = void* (__fastcall*)(void* self, int group, int record);

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string ReadAll(const std::wstring& p) {
    std::ifstream f(p.c_str(), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// 用生产代码同一套 API 写文件（_wfopen_s），失败要看得见
static bool WriteTextFile(const std::wstring& path, const char* text) {
    FILE* f = nullptr;
    const errno_t e = ::_wfopen_s(&f, path.c_str(), L"wb");
    if (e != 0 || !f) { std::printf("       debug: 写 %ls 失败 errno=%d\n", path.c_str(), (int)e); return false; }
    std::fputs(text, f);
    std::fclose(f);
    return true;
}

int main() {
    std::printf("=== SonarEfx 离线自检 ===\n");

    // 0) 写入跳板字节并校验自洽
    {
        unsigned char* p = g_blob + kThunkOff;
        DWORD old = 0;
        ::VirtualProtect(g_blob, sizeof(g_blob), PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE9; p[1] = 0xFB; p[2] = 0xC8; p[3] = 0xFF; p[4] = 0xFF;
        ::VirtualProtect(g_blob, sizeof(g_blob), old, &old);

        const int rel = *reinterpret_cast<int*>(p + 1);
        const unsigned char* target = p + 5 + rel;
        Check(p[0] == 0xE9 && p[1] == 0xFB && p[2] == 0xC8 && p[3] == 0xFF && p[4] == 0xFF,
              "跳板字节 = E9 FB C8 FF FF（与 vfx-forge 记录一致）");
        Check(rel == -14085, "rel32 = -14085（0x3705，与游戏里那条一致）");
        Check(target == g_blob, "跳板目标解析正确（rel32 指回 stub）");
        Check(target[0] == 0x48 && target[1] == 0x83 && target[2] == 0xEC,
              "目标处是合法函数序言（48 83 EC …）");
    }

    // 1) 临时目录 + 迷你索引（hm_wp03 武器表 + 一张干扰表）
    // 工作目录放在 exe 旁边（%TEMP% 在本机被沙箱拒绝写，errno=13）
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self;
    const std::size_t sl = dir.find_last_of(L"\\/");
    dir = (sl == std::wstring::npos) ? std::wstring() : dir.substr(0, sl + 1);
    dir += L"selftest_tmp";
    if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD ec = ::GetLastError();
        if (ec != ERROR_ALREADY_EXISTS) std::printf("       debug: CreateDirectory 失败 err=%lu\n", ec);
    }
    dir += L"\\";
    const std::string idx =
        "#TABLE|hm_wp03|wp\\swo\\epv\\hm_wp03\n"
        "hm_wp03\t2005\t0\t-1\tvfx\\efx\\wp\\wp03\\wp03_050\n"
        "hm_wp03\t2005\t1\t-1\tvfx\\efx\\wp\\wp03\\wp03_050\n"
        "hm_wp03\t2005\t2\t-1\tvfx\\efx\\wp\\wp03\\wp03_050\n"
        "hm_wp03\t2001\t0\t-1\tvfx\\efx\\wp\\wp03\\wp03_010\n"
        "#TABLE|gm_em_hole|common\\em\\gm_em_hole\\epv\\gm_em_hole\n"
        "gm_em_hole\t9999\t9\t-1\tvfx\\efx\\cm\\cm_em\\ground\\cm_em_remove_100\n";
    const std::wstring idxPath = dir + L"WseEfxIndex.txt";
    const bool idxOk = WriteTextFile(idxPath, idx.c_str());
    ::DeleteFileW((dir + L"WeaponSoundEnhance_efx.log").c_str());   // 从干净状态开始
    Check(idxOk, "测试用索引已写入临时目录");
    Check(::GetFileAttributesW(idxPath.c_str()) != INVALID_FILE_ATTRIBUTES, "索引文件确实存在");

    // 2) 初始化（扫描本 exe 的可执行段 + 挂钩跳板）
    std::string log;
    const bool ok = ::sonarefx::Init(dir, log);
    Check(ok, "Init 返回成功");
    Check(::sonarefx::Available(), "Available() == true（挂钩成功）");
    std::printf("       Status: %s\n", ::sonarefx::Status().c_str());
    Check(::sonarefx::Status().find("挂钩成功") != std::string::npos, "Status 里写明挂钩成功");
    if (!ok) { std::printf("\n结果: 失败（初始化没成功）\n"); return 1; }

    // 3) 调用跳板（等价于游戏生成特效）
    FnEffectCtor call = reinterpret_cast<FnEffectCtor>(g_blob + kThunkOff);
    call(nullptr, 2005, 2);      // 武器特效 → 记录
    call(nullptr, 2005, 2);      // 同标识连续 → 去重
    call(nullptr, 9999, 9);      // 非武器表 → 丢弃
    call(nullptr, 2001, 0);      // 另一条武器特效
    ::sonarefx::Flush();

    const ::sonarefx::Stats st = ::sonarefx::GetStats();
    std::printf("       stats: calls=%llu weapon=%llu other=%llu dup=%llu lines=%llu\n",
                st.calls, st.weapon, st.other, st.dup, st.logLines);
    Check(st.calls == 4, "hook 收到 4 次调用");
    Check(st.weapon == 3, "其中 3 次判定为武器特效");
    Check(st.other == 1, "1 次非武器特效被丢弃");
    Check(st.dup == 1, "1 次重复被去重");

    // 4) 日志内容
    const std::string text = ReadAll(dir + L"WeaponSoundEnhance_efx.log");
    std::printf("       log:\n");
    {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) std::printf("         %s\n", line.c_str());
    }
    Check(text.find("hook=ok") != std::string::npos, "日志头部写了 hook=ok");
    Check(text.find("table=hm_wp03 group=2005 rec=2") != std::string::npos, "记录了 (hm_wp03, 2005, 2)");
    Check(text.find("table=hm_wp03 group=2001 rec=0") != std::string::npos, "记录了 (hm_wp03, 2001, 0)");
    Check(text.find("gm_em_hole") == std::string::npos, "非武器表没写进日志");
    // 诊断模式下非武器调用会写 efxraw 行（便于定位"值不在索引里"），但绝不能混进 efx 行
    Check(text.find("efx seq#2 table=gm_em_hole") == std::string::npos, "非武器调用没有混进 efx 行");
    Check(text.find("efxraw group=9999 rec=9") != std::string::npos,
          "诊断模式记录了原始 (组,记录)（efxraw）");
    Check(text.find("# stats calls=4 weapon=3 other=1 dup=1") != std::string::npos,
          "统计行内容正确（calls/weapon/other/dup）");

    // 5) 开关
    ::sonarefx::SetEnabled(false);
    call(nullptr, 2005, 1);
    ::sonarefx::Flush();
    Check(::sonarefx::GetStats().calls == 4, "SetEnabled(false) 后不再记账");
    ::sonarefx::SetEnabled(true);

    // 6) 破坏特征码 → 必须明确失败（不静默）
    ::sonarefx::Shutdown();
    {
        DWORD old = 0;
        ::VirtualProtect(g_blob, sizeof(g_blob), PAGE_EXECUTE_READWRITE, &old);
        g_blob[kThunkOff] = 0x90;   // E9 → NOP：特征码消失
        ::VirtualProtect(g_blob, sizeof(g_blob), old, &old);
        std::string log2;
        const bool ok2 = ::sonarefx::Init(dir, log2);
        Check(!ok2, "特征码被破坏后 Init 返回失败（不静默）");
        Check(::sonarefx::Status().find("未找到") != std::string::npos ||
              ::sonarefx::Status().find("失败") != std::string::npos, "Status 写明失败原因");
        std::printf("       Status: %s\n", ::sonarefx::Status().c_str());
    }

    std::printf("\n结果: %s（失败 %d 项）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
