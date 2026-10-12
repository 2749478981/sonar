#pragma once
#include "config.h"
#include "game.h"
#include <string>
#include <vector>
#include <atomic>

// 当前 GUI 版本（显示在界面上、也用于和仓库最新版比较）
inline constexpr const char* kWseGuiVersion = "2.37.7";

// 条件表达式里的一项：<变量> <比较符> <数值>
struct CondTerm {
    int  var = 0;          // 下标见 app.cpp 的 kCondVars
    int  op  = 0;          // 下标见 app.cpp 的 kCondOps
    int  val = 0;
    bool orBefore = false; // 与前一项之间是 |（或），false = &（且）
};

// 编辑器里的一条「条件 -> 音效池」
struct CondRow {
    std::vector<CondTerm> terms;
    std::string rawExpr;    // terms 解析不出来时保留原文，界面转成只读文本框
    bool parsed = true;
    bool atEnd = false;     // 只在窗口结束时评（写成 SoundEnd:）
    std::string label;      // 预设给的友好名，如"成功"/"失败(掉刃)"
    std::vector<SoundSpec> pool;
};

struct App {
    App();

    void Draw();

    void* hwnd = nullptr;      // 主窗口 HWND
    void* editHwnd = nullptr;  // 独立编辑窗口 HWND（main.cpp 创建编辑窗口后赋值；文件对话框后用于把编辑窗口带回最前）
    float dpiScale = 1.0f;

    // 主窗口那台 D3D11 设备（main.cpp 注入）——界面图标要建纹理。
    // 为空时界面只是不画图标，功能不受影响。
    void* d3dDevice = nullptr;

    Config cfg;
    GameReader game;
    LiveState live;
    bool liveOk = false;
    bool mDirty = false;

    int weaponFilter = -1;     // -1=全部 -2=任意武器 0..13
    char searchBuf[160] = {};

    struct Editor {
        bool open = false;
        bool isNew = false;
        int index = -1;        // cfg.entries 索引
        char name[128] = {};
        char groupBuf[96] = {};   // 动作组（Group=）
        int weaponType = -1;
        int fsmId = -1;
        int fsmTarget = -1;       // FSMTarget=：-1 = 不限定
        std::vector<int> lmt;          // 空 = 不限
        char lmtBuf[128] = {};
        bool lmtAny = false;           // 勾选「LMT 不限」= 该 FSMId 的所有动作都触发
        std::string combo;             // 条目所属配置组合（"" = 默认组合），编辑器里可改
        bool stop = false;             // Stop=1：命中此动作时停止正在播放的音效
        bool advOpen = false;          // 「高级设置」折叠区是否展开（默认折叠）
        std::vector<SoundSpec> pool[5]; // 0 = 默认音效；1..4 = 无刃时/白刃时/黄刃时/红刃时

        // ---- 判定（延迟判定）----
        int  judgePreset = 0;        // 0=不判定 1..N=内置预设 N+1=自定义
        bool advanced = false;       // 展开高级设置
        int  checkDelayMs = 0;
        int  checkTimeoutMs = 0;
        int  checkOffsetMs = 150;
        bool endOnAction = true;
        int  checkMode = 0;          // 1 = CheckMode=final
        std::vector<CondRow> conds;
    } editor;

    bool mRaiseEditor = false;   // 请求把编辑窗置顶（main.cpp 泵里执行）
    bool fsmWinOpen = false;   // 启动时不弹 FSM 查询窗（点工具栏「FSM 查询」再开）
    std::string mGameIni;      // 游戏实际读取的那份 ini（<游戏目录>\nativePC\plugins\WeaponSoundEnhance\...）
    bool idWinOpen = false;    // 共享动作 ID 库窗口（点工具栏「上传ID」打开）
    char fsmQuery[160] = {};
    int fsmWeaponFilter = -1;

    // ---- 版本 / 在线更新状态 ----
    // mUpdState: 0=未查 1=查询中 2=已是最新 3=有新版 4=出错
    int         mUpdState = 0;
    bool        mUpdWinOpen = false;
    bool        mUpdUserAsked = false;      // 用户手动点过「检查更新」（出错时才弹提示）
    bool        mUpdInstalling = false;
    int         mUpdProgress = 0;           // 0..100
    std::string mUpdTag, mUpdUrl, mUpdNotes, mUpdErr, mUpdMsg;
    bool        mUpdNeedRestart = false;
    bool        mUpdIniPending = false;     // 更新包里带 ini → 等主线程合并进用户配置（不替换）
    bool        mUpdAutoChecked = false;
    std::string mUpdateProxy;                // ini: UpdateProxy=  (如 http://127.0.0.1:7897)
    int         mUpdateCheckEnabled = 1;     // ini: UpdateCheck=1

    // 抓取历史：记录 fsm/lmt/weapon 的变化（fsm≠0 的派生动作会被高亮）
    struct HistEntry {
        int fsm;
        int lmt;
        int weapon;
        int weaponId;
        std::string time;      // "HH:MM:SS"
    };
    std::vector<HistEntry> history;
    bool histExpanded = false;   // 实时捕获历史：展开显示全部

    std::string status;

    // 在独立编辑窗口（第二个 ImGui 上下文）中绘制编辑内容；由 main.cpp 的编辑窗渲染循环调用
    void DrawEditorDetached();
    void DrawFsmWindow();   // FSM/LMT 查询（独立原生窗口里绘制）
    // 一条音效的编辑卡片（路径/固定/延时/音量/冷却/播放期间不重复/试听）。
    // 普通音效池和判定条件池共用，保证两边的可调项完全一致。
    // 返回 0=无操作 1=请求换文件 2=请求移除（调用方负责改容器）。
    int DrawSoundSpecRow(SoundSpec& sp, int index, float dpiScale);
    void OpenEditorNew(int weapon, int fsm, int lmt, const std::string& name);
    void OpenEditorNew(int weapon);
    void OpenEditorEdit(int index);

private:
    unsigned long long mLastPoll = 0;
    float mSaveFlash = 0.0f;   // 保存成功提示的剩余显示时间(秒)
    void PollGame();
    void RecordHistory();
    void DrawToolbar();
    void DrawWeaponTree();
    void DrawCapturePanel();
    void DrawEntries();
    void DrawStatus();
    bool ApplyEditor();   // false = 输入有问题（例如 LMT 填了无法识别的项），不落地
    void DrawIdShareWindow();   // 共享动作 ID 库（上传/获取）
    void Save();
    void SaveAs();
    void Load(const std::string& path);
    std::string OpenFileDialogIni();
    int BrowseSounds(std::vector<SoundSpec>& out);   // 追加选中的 wav 为默认属性音效
    std::string RelPathForPicked(const std::wstring& f, bool& copiedFallback);   // 选中音效 → 要存的路径
    std::string PickSoundFile();   // 单文件音效选择框，返回要存的路径（取消返回空）
    void PlaySoundPreview(const std::string& rel, int vol, int delayMs);
    int CountFor(int w) const;
    void EnrichNames();
    std::string BaseDir() const;
    std::string ResolveName(int weapon, int fsm, int lmt) const;
    bool IsCapturedAdded(int weapon, int fsm, int lmt) const;
    // 每武器当前激活的组合名（""=默认）
    std::string ActiveCombo(int w) const;
    // 该条目是否属于"当前激活组合"（非激活条目不显示/不参与匹配）
    bool EntryActive(const SoundEntry& e) const;
    // 共享动作 ID 库（fsm_db.csv）：导出实测 / 导入合并 / 提交 / 获取最新
    std::string OpenFileDialogCsv();
    void ExportMeasuredIdsCsv();
    void ImportIdsCsv();
    void SubmitIdsToGithub();
    void FetchLatestFsmDb();
    void MergeOldIni();   // 合并旧版 ini 的动作条目（升级不丢配置）
    void MergeConfigFile(const std::string& path, bool importCombo = false);
    // 合并指定 ini：importCombo=true（组合导入/打包导入）时，导入的"默认组合"条目
    // 会被放进自动新建的「导入」组合，绝不覆盖本地默认组合
    void ExportComboCurrent();   // 把当前武器的当前组合导出为分享文件
    void ImportComboFile();      // 导入别人分享的组合文件（按条目去重合并）
    // 提取组合条目 + 把用到的音效复制到 dir\sounds\ 并改写路径（可测试的导出核心）
    bool PackComboToDir(int w, const std::string& combo, const std::string& dir,
                        std::vector<SoundEntry>& outEntries, int& copied, int& missing);
    void SwitchToGameIni();   // 切到游戏实际读取的 ini（当前那份不是它时用）
    // ---- 版本 / 在线更新 ----
    void CheckUpdateAsync(bool userInitiated);   // 后台线程查最新 release
    void DrawUpdateWindow();                     // 「关于/更新」小窗口
    void StartInstallUpdate();                   // 下载 + 解包 + 覆盖安装（后台线程）
    // ---- 配置组合管理 ----
    std::vector<std::string> WeaponCombos(int w) const;              // "" 在最前 + 命名组合(按出现顺序)
    void ComboRename(int w, const std::string& from, const std::string& to);
    void ComboDelete(int w, const std::string& name);
    void ComboMove(int w, const std::string& name, int dir);         // dir=-1 上移 / +1 下移
};
