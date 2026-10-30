#pragma once
// ===========================================================================
//  core.h —— Sonar GUI 的"无界面业务核心"
//
//  背景：原来的界面是 Dear ImGui（gui/src/app.cpp，3400 行），界面和业务逻辑
//  混在一起。现在界面改成 WebView2 + HTML/CSS/JS，所以把**界面之外**的一切
//  （配置读写、组合、导入导出、FSM 库、实时捕获、在线更新、界面偏好持久化）
//  收进这个 Core。
//
//  ★ 设计要点：对外只暴露 **一个入口** Handle()，收/发都是 JSON 字符串。
//    这样宿主（gui/src/webview_host.cpp）完全不需要知道内部结构，
//    界面侧也只按 method 名字调用。加功能不用改宿主。
//
//  所有 Handle 都返回 JSON：
//      成功  {"ok":true,  "data": <任意>}
//      失败  {"ok":false, "error":"原因"}
//
//  参数也是 JSON 对象字符串；无参数传 "{}" 或 ""。
//  【重要】不要抛异常穿过边界；内部错误一律转成 ok:false + error。
// ===========================================================================

#include <string>

class Core {
public:
    Core();
    ~Core();

    // 文件对话框等需要父窗口的操作依赖它（main 里注入）。
    void SetOwnerWindow(void* hwnd) { mOwner = hwnd; }

    // 唯一入口。method 见下方清单；paramsJson 是 JSON 对象，可为空。
    std::string Handle(const std::string& method, const std::string& paramsJson);

    // 定时（约 100ms 一次）由宿主调用。
    // 返回非空字符串时，宿主会把它当作事件对象 postMessage 给前端：
    //   {"event":"live","data":{...}} / {"event":"update","data":{...}}
    //   {"event":"status","data":{"text":"..."}}
    // 没有事件返回空串。
    std::string PollEvents();

    // 内嵌资源（武器图标 / logo / 成就音效）的解压目录（UTF-8）。
    // 宿主拿它给 WebView2 做 https://sonar.assets 映射 —— 必须和 core 用的是
    // 同一个目录，不要自己再去读 LOCALAPPDATA（两处算不一致时图标会全 404）。
    std::string AssetsDir();

    // ---------------------------------------------------------------
    //  method 清单（params 里出现的键名就是这些）
    // ---------------------------------------------------------------
    //
    // ---- 基础状态 ----
    //  state              {}                       -> {version, exeDir, dataDir,
    //                                                 iniPath, gameIniPath,
    //                                                 isGameIni, ui:{...},
    //                                                 global:{...}, hotkeys:{...},
    //                                                 active:{weapon:combo},
    //                                                 entryCount, weapons:[{id,name,icon,count}],
    //                                                 theme, defaultTheme}
    //
    // ---- 配置读写 ----
    //  config.load        {path?}                 -> {iniPath, warnings:[...], fixedCombos}
    //  config.save        {}                      -> {iniPath}
    //  config.saveAs      {}                      -> {iniPath, cancelled?}
    //  config.open        {}                      -> {iniPath, cancelled?}   打开对话框
    //  config.switchToGameIni {}                  -> {iniPath}
    //  config.mergeOldIni {}                      -> {added}
    //
    // ---- 全局设置 ----
    //  global.set         {key, value}            -> {}   key ∈ GlobalSettings 字段名
    //  hotkeys.set        {key, value}            -> {}
    //
    // ---- 条目 ----
    //  entries.list       {weapon?, combo?, q?}   -> {entries:[Entry...], total}
    //  entries.get        {index}                 -> {entry:Entry, index}
    //  entries.save       {index?, entry}         -> {index}      index 缺省 = 新增
    //  entries.remove     {indices:[int]}         -> {removed}
    //  entries.duplicate  {index}                 -> {index}
    //
    //  Entry JSON（与 config.h 的 SoundEntry 对应）：
    //   {index, name, weaponType, combo, fsmId, fsmTarget, group, stop,
    //    lmt:[int], lmtAny:bool,
    //    def:[SoundSpec...], gauge:[[...],[...],[...],[...]],
    //    checkDelayMs, checkTimeoutMs, checkOffsetMs, endOnAction, checkMode,
    //    judgePreset, conds:[{expr, atEnd, label, pool:[SoundSpec...]}]}
    //  SoundSpec JSON：{path, delay, vol, fixed, cdMs, playLock}
    //
    // ---- 配置组合 ----
    //  combo.list         {weapon}                -> {combos:["", "小指良", ...], active}
    //  combo.create       {weapon, name}          -> {}
    //  combo.rename       {weapon, from, to}      -> {}
    //  combo.remove       {weapon, name}          -> {removedEntries}
    //  combo.move         {weapon, name, dir}     -> {}
    //  combo.setActive    {weapon, name}          -> {}
    //
    // ---- 音效文件 ----
    //  sound.pick         {}                      -> {path, cancelled?}   单选
    //  sound.pickMulti    {}                      -> {paths:[...], cancelled?}
    //  sound.preview      {path, vol, delay}      -> {}
    //  sound.openSoundsDir{}                      -> {}
    //  sound.exists       {path}                  -> {exists}
    //
    // ---- FSM / LMT 知识库 ----
    //  fsm.search         {q?, weapon?}           -> {rows:[{name, weapon, lmt, fsm,
    //                                                 source}], total}
    //  fsm.exportMeasured {}                      -> {path, count}
    //  fsm.importCsv      {}                      -> {added, path, cancelled?}
    //  fsm.submit         {}                      -> {path, url}
    //  fsm.fetchLatest    {}                      -> {path, count}
    //  fsm.captured       {}                      -> {captured:[{weapon,fsm,lmt,name,added}]}
    //
    // ---- 组合包分享 ----
    //  combo.export       {}                      -> {zip, copied, missing, cancelled?}
    //  combo.import       {}                      -> {added, copied, cancelled?}
    //
    // ---- 游戏交互 ----
    //  game.launch        {}                      -> {launched, running}
    //  game.reload        {}                      -> {}     写 _wse_reload.flag
    //  game.stop          {}                      -> {}     写 _wse_stop.flag
    //
    // ---- 在线更新 ----
    //  update.check       {user?}                 -> {state, tag, url, notes, err}
    //  update.install     {}                      -> {started}
    //  update.state       {}                      -> {state, progress, msg, needRestart}
    //  update.openLatest  {}                      -> {}
    //
    // ---- 界面偏好（替代原 ImGui 的 ini 记忆：主题/列宽/排序/窗口尺寸）----
    //  ui.get             {}                      -> {theme, colWidths, sort, window,
    //                                                 weaponFilter, histExpanded}
    //  ui.set             {任意子集}               -> {}     并立即落盘
    //  theme.set          {theme}                 -> {theme}  theme ∈
    //                                                 "clean-light"(默认) | "pro-dark"
    //                                                 | "cute-pastel" | "sticker-outline"
    //  theme.get          {}                      -> {theme, themes:[{id,name,desc}]}

    // ---- 内部实现 ----
    // ★ Impl 的**定义**在 core.cpp（PIMPL）。这里公开声明 + 公开指针，是为了让
    //   core.cpp 里那些静态辅助函数（文件对话框、压缩包、ini 合并…）能自然地写
    //   Core::Impl*。它们不属于任何类，没法通过成员函数拿 mImpl。
    //   【踩过的坑】曾经想"不改头文件"，用 reinterpret_cast 去偷 mImpl ——
    //   那种布局投机一旦算错偏移就是越界读，必崩。公开访问权限才是正解。
    struct Impl;
    Impl* mImpl = nullptr;
    void* mOwner = nullptr;
};
