/* ===========================================================================
   mock.js —— 后端抽象层
   两种模式：
     ① WebView2 模式：window.chrome.webview 存在 → 走真实的 C++ Core
     ② 浏览器开发模式：不存在 → 用本文件里的假数据，界面照样能跑
        （这样调界面不用每次编译 C++，也能直接用 Edge 预览）
   统一入口：Backend.call(method, params) -> Promise<data>
             Backend.onEvent(cb)          cb({event, data})
   =========================================================================== */
(function () {
  'use strict';

  const hasWebView = !!(window.chrome && window.chrome.webview);
  const pending = new Map();
  const eventHandlers = [];
  let nextId = 1;

  /* ---------------- WebView2 模式 ---------------- */
  if (hasWebView) {
    window.chrome.webview.addEventListener('message', (ev) => {
      let msg = ev.data;
      if (typeof msg === 'string') { try { msg = JSON.parse(msg); } catch { return; } }
      if (!msg || typeof msg !== 'object') return;
      if (msg.event) { eventHandlers.forEach(f => f(msg)); return; }
      const p = pending.get(msg.id);
      if (!p) return;
      pending.delete(msg.id);
      if (msg.ok) p.resolve(msg.data);
      else p.reject(new Error(msg.error || '未知错误'));
    });
  }

  // 打开系统文件/文件夹对话框时，宿主 UI 线程被这个模态对话框占住，
  // 期间任何请求都不会被处理。用户挑文件可能花好几分钟 —— 用 30 秒超时
  // 就会在对话框还没关掉时先报"后端无响应"（jsErr 里真的抓到过
  // "unhandled: 后端无响应：bank.pick"）。带 pick/browse 的方法一律放宽。
  const DIALOG_TIMEOUT = 900000;

  const Backend = {
    mode: hasWebView ? 'webview2' : 'browser',
    onEvent(cb) { eventHandlers.push(cb); },
    call(method, params) {
      if (!hasWebView) return Mock.call(method, params || {});
      const waitMs = /pick|browse/i.test(method) ? DIALOG_TIMEOUT : 30000;
      return new Promise((resolve, reject) => {
        const id = nextId++;
        pending.set(id, { resolve, reject });
        window.chrome.webview.postMessage(JSON.stringify({ id, method, params: params || {} }));
        setTimeout(() => {
          if (pending.has(id)) { pending.delete(id); reject(new Error('后端无响应：' + method)); }
        }, waitMs);
      });
    },
  };

  /* ===========================================================================
     浏览器开发模式：假数据 + 假后端
     =========================================================================== */
  // 只给 id 和名字 —— 和真实后端（core.cpp）保持一致。
  // 武器图标文件名由 app.js 的 ICON_FILES 统一决定，避免两处各写一份对不上。
  const WEAPONS = [
    { id: 0,  name: '大剑' },
    { id: 1,  name: '片手' },
    { id: 2,  name: '双刀' },
    { id: 3,  name: '太刀' },
    { id: 4,  name: '大锤' },
    { id: 5,  name: '笛子' },
    { id: 6,  name: '长枪' },
    { id: 7,  name: '铳枪' },
    { id: 8,  name: '斩斧' },
    { id: 9,  name: '盾斧' },
    { id: 10, name: '虫棍' },
    { id: 11, name: '弓箭' },
    { id: 12, name: '重弩' },
    { id: 13, name: '轻弩' },
  ];

  const THEMES = [
    { id: 'clean-light',     name: '极简浅色', desc: '白底细边，最清爽（默认）' },
    { id: 'pro-dark',        name: '深色专业', desc: '中性深灰，信息密度最高' },
    { id: 'cute-pastel',     name: '可爱风',   desc: '马卡龙色，圆角胶囊' },
    { id: 'sticker-outline', name: '描边贴纸', desc: '和武器图标同款笔触' },
  ];

  const JUDGES = [
    { i: 0, name: '不判定' },
    { i: 1, name: '造成伤害（命中/落空·任意武器）' },
    { i: 2, name: '太刀 · 登龙 命中/落空' },
    { i: 3, name: '太刀 · 大居 成功/失败' },
    { i: 4, name: '大剑 · 真蓄 命中/落空' },
    { i: 5, name: '自定义（自己写条件）' },
  ];

  const S = (path, delay, vol, opts) => Object.assign(
    { path, delay: delay || 0, vol: vol == null ? 100 : vol, fixed: false, cdMs: 0, playLock: false },
    opts || {});

  function mkEntry(o) {
    return Object.assign({
      index: 0, name: '', weaponType: -1, combo: '', fsmId: -1, fsmTarget: -1,
      group: '', stop: false, lmt: [], lmtAny: true,
      def: [], gauge: [[], [], [], []],
      checkDelayMs: 0, checkTimeoutMs: 0, checkOffsetMs: 150,
      endOnAction: true, checkMode: 0, judgePreset: 0, conds: [],
    }, o);
  }

  const state = {
    theme: 'clean-light',
    // 与 config.h 的 Hotkeys 默认值一致（键值是 Windows VK 码）
    hotkeys: { modifierKey: 17, reloadKey: 116, volUpKey: 38, volDownKey: 40,
               setVolKey: 119, setVolValue: 50, toggleKey: 120, moreKey: 121, comboKey: 122 },
    ui: { colWidths: {}, sort: { key: 'name', asc: true }, weaponFilter: -1, histExpanded: false, window: null },
    cfgPath: 'F:\\SteamLibrary\\steamapps\\common\\Monster Hunter World\\nativePC\\plugins\\WeaponSoundEnhance\\WeaponSoundEnhance.ini',
    global: { enabled: 1, moreSounds: 1, volume: 50, debug: 0, hotkeysEnabled: 1, updateCheck: 1 },
    active: { 3: '', 0: '' },
    combos: {
      3: ['', '小指良', '白刃流'],
      0: [''],
    },
    entries: [
      mkEntry({ name: '白槽大居', weaponType: 3, combo: '小指良', fsmId: 102, lmt: [49461], lmtAny: false,
        def: [S('sounds/小指良/技能3第二段.mp3', 0, 100)] }),
      mkEntry({ name: '大居', weaponType: 3, combo: '小指良', fsmId: 102, lmt: [49460], lmtAny: false,
        def: [S('sounds/小指良/技能3第二段.mp3', 0, 100)] }),
      mkEntry({ name: '登龙飞天', weaponType: 3, combo: '小指良', fsmId: 90, lmt: [49324], lmtAny: false,
        def: [S('sounds/小指良/硬币正面.wav', 0, 100, { fixed: true })] }),
      mkEntry({ name: '气刃兜割', weaponType: 3, combo: '小指良', fsmId: 92, lmt: [49326], lmtAny: false,
        checkTimeoutMs: 2500, judgePreset: 1,
        def: [S('sounds/小指良/硬币正面.wav', 0, 100, { fixed: true })],
        conds: [{ expr: 'dmg>0', atEnd: false, label: '命中',
                  pool: [S('sounds/小指良/杀死环指.mp3', 120, 80, { cdMs: 800, playLock: true })] }] }),
      mkEntry({ name: '见切', weaponType: 3, combo: '小指良', fsmId: 86, lmt: [49292], lmtAny: false,
        def: [S('sounds/小指良/硬币正面.wav', 0, 100, { fixed: true })] }),
      mkEntry({ name: '逆袈裟', weaponType: 3, combo: '小指良', fsmId: 70, lmt: [49262], lmtAny: false,
        def: [S('sounds/小指良/硬币正面.wav', 0, 100, { fixed: true })] }),
      mkEntry({ name: '磨刀', weaponType: 3, combo: '小指良', fsmId: 532, lmt: [49222], lmtAny: false,
        def: [S('sounds/小指良/无我良秀.wav', 0, 100)] }),
      mkEntry({ name: '软化', weaponType: -1, combo: '', fsmId: 4, lmtAny: true,
        group: '软化', def: [S('sounds/软化/attack.wav', 0, 100, { fixed: true })] }),
      mkEntry({ name: '真蓄力斩', weaponType: 0, combo: '', fsmId: 118, lmt: [49298], lmtAny: false,
        checkDelayMs: 1200, checkTimeoutMs: 3500, judgePreset: 4,
        def: [S('sounds/大剑/真蓄.wav', 0, 100)],
        conds: [{ expr: 'dmg>0', atEnd: false, label: '命中', pool: [S('sounds/大剑/命中.wav', 0, 100)] }] }),
      mkEntry({ name: '超解', weaponType: 9, combo: '', fsmId: 210, lmt: [49421], lmtAny: false,
        def: [S('sounds/盾斧/超解.wav', 0, 100, { fixed: true })] }),
      // 只有判定音效、没有默认音效的条目：主列表应显示判定音效而不是"无音效"
      mkEntry({ name: '仅判定音效示例', weaponType: -1, combo: '', fsmId: 90, lmtAny: true,
        def: [],
        conds: [{ expr: 'dmg>0', atEnd: false, label: '命中',
                  pool: [S('sounds/示例/命中判定.wav', 0, 100, { fixed: true })] }] }),
    ],
    history: [],
    attached: false,
  };
  state.entries.forEach((e, i) => e.index = i);

  const FSM_ROWS = [
    { name: '气刃兜割', weapon: 3, lmt: 49326, fsm: 92,  source: '基础库' },
    { name: '登龙飞天', weapon: 3, lmt: 49324, fsm: 90,  source: '基础库' },
    { name: '见切斩',   weapon: 3, lmt: 49292, fsm: 86,  source: '基础库' },
    { name: '真蓄力斩', weapon: 0, lmt: 49298, fsm: 118, source: '实测' },
    { name: '超解',     weapon: 9, lmt: 49421, fsm: 210, source: '实测' },
    { name: '飞天盾击', weapon: 9, lmt: 49423, fsm: 214, source: '实测' },
  ];

  function delay(ms, v) { return new Promise(r => setTimeout(() => r(v), ms)); }

  const Mock = {
    async call(method, p) {
      switch (method) {
        case 'state':
          return {
            version: '2.34', theme: state.theme, defaultTheme: 'clean-light',
            iniPath: state.cfgPath, isGameIni: true,
            assetBase: '../../', iconBase: '../../weapons_icons/',
            logoUrl: '../../gui/sonar_icon.png',
            weapons: WEAPONS.map(w => ({
              ...w,
              count: state.entries.filter(e => e.weaponType === w.id).length,
              activeCount: state.entries.filter(e => e.weaponType === w.id &&
                       (e.combo === (state.active[w.id] || ''))).length,
            })),
            anyCount: state.entries.filter(e => e.weaponType < 0).length,
            entryCount: state.entries.length,
            global: state.global, active: state.active, hotkeys: state.hotkeys,
            judges: JUDGES, themes: THEMES,
            ui: state.ui,
          };
        case 'ui.set':
          Object.assign(state.ui, p); return {};
        case 'theme.get':
          return { theme: state.theme, themes: THEMES };
        case 'theme.set':
          state.theme = p.theme; return { theme: state.theme };

        case 'entries.list':
          return { entries: state.entries.map(e => ({ ...e })), total: state.entries.length };
        case 'entries.get':
          return { entry: JSON.parse(JSON.stringify(state.entries[p.index])), index: p.index };
        case 'entries.save': {
          const e = p.entry;
          if (p.index == null) { e.index = state.entries.length; state.entries.push(e); }
          else { e.index = p.index; state.entries[p.index] = e; }
          return { index: e.index };
        }
        case 'entries.remove': {
          const idx = [...p.indices].sort((a, b) => b - a);
          idx.forEach(i => state.entries.splice(i, 1));
          state.entries.forEach((e, i) => e.index = i);
          return { removed: idx.length };
        }
        case 'entries.duplicate': {
          const c = JSON.parse(JSON.stringify(state.entries[p.index]));
          c.name = (c.name || '条目') + ' 副本';
          state.entries.splice(p.index + 1, 0, c);
          state.entries.forEach((e, i) => e.index = i);
          return { index: p.index + 1 };
        }

        case 'combo.list':
          return { combos: (state.combos[p.weapon] || ['']).slice(), active: state.active[p.weapon] || '' };
        case 'combo.setActive':
          state.active[p.weapon] = p.name; return {};
        case 'combo.create':
          (state.combos[p.weapon] = state.combos[p.weapon] || ['']).push(p.name); return {};
        case 'combo.rename': {
          const l = state.combos[p.weapon]; const i = l.indexOf(p.from);
          if (i >= 0) l[i] = p.to;
          state.entries.forEach(e => { if (e.weaponType === p.weapon && e.combo === p.from) e.combo = p.to; });
          if (state.active[p.weapon] === p.from) state.active[p.weapon] = p.to;
          return {};
        }
        case 'combo.remove': {
          const l = state.combos[p.weapon]; const i = l.indexOf(p.name);
          if (i >= 0) l.splice(i, 1);
          const before = state.entries.length;
          state.entries = state.entries.filter(e => !(e.weaponType === p.weapon && e.combo === p.name));
          state.entries.forEach((e, k) => e.index = k);
          if (state.active[p.weapon] === p.name) state.active[p.weapon] = '';
          return { removedEntries: before - state.entries.length };
        }
        case 'combo.move': {
          const l = state.combos[p.weapon]; const i = l.indexOf(p.name);
          const j = i + p.dir;
          if (i > 0 && j > 0 && j < l.length) { const t = l[i]; l[i] = l[j]; l[j] = t; }
          return {};
        }

        case 'fsm.search': {
          const q = (p.q || '').trim().toLowerCase();
          let rows = FSM_ROWS.slice();
          if (p.weapon != null && p.weapon >= 0) rows = rows.filter(r => r.weapon === p.weapon);
          if (q) rows = rows.filter(r => (r.name && r.name.toLowerCase().includes(q)) ||
                                          String(r.fsm).includes(q) || String(r.lmt).includes(q));
          return { rows, total: rows.length };
        }
        case 'fsm.captured':
          return { captured: [] };

        case 'global.set':
          state.global[p.key] = p.value; return {};
        case 'hotkeys.set':
          if (p.key in state.hotkeys) state.hotkeys[p.key] = p.value;
          return Object.assign({}, state.hotkeys);
        case 'sound.exists':
          return { exists: true };
        case 'sound.preview':
          return {};

        case 'config.save':  return { iniPath: state.cfgPath };
        case 'config.load':  return { iniPath: state.cfgPath };

        default:
          return delay(120, {});
      }
    },
  };

  /* ---- 开发模式下模拟实时捕获事件，方便看右栏效果 ---- */
  if (!hasWebView) {
    const seq = [
      { fsm: 86,  lmt: 49292 }, { fsm: 92, lmt: 49326 }, { fsm: 90, lmt: 49324 },
      { fsm: 102, lmt: 49460 }, { fsm: 102, lmt: 49461 }, { fsm: 532, lmt: 49222 },
    ];
    let k = 0;
    setInterval(() => {
      const s = seq[k++ % seq.length];
      const now = new Date();
      // 和真实 core 的 HistoryJson 保持一致：带上解析出的动作名 + 是否已有条目
      const row = FSM_ROWS.find(x => x.fsm === s.fsm && x.lmt === s.lmt) ||
                  FSM_ROWS.find(x => x.fsm === s.fsm);
      state.history.unshift({
        weapon: 3, fsm: s.fsm, lmt: s.lmt,
        name: row ? row.name : undefined,
        added: state.entries.some(e => e.weaponType === 3 && e.fsmId === s.fsm),
        time: [now.getHours(), now.getMinutes(), now.getSeconds()]
          .map(n => String(n).padStart(2, '0')).join(':'),
      });
      if (state.history.length > 64) state.history.pop();
      eventHandlers.forEach(f => f({
        event: 'live',
        data: { attached: true, inScene: true, pid: 64912, weapon: 3, weaponId: 17,
                fsm: s.fsm, lmt: s.lmt, history: state.history.slice(0, 20) },
      }));
    }, 2600);
  }

  window.Backend = Backend;
})();
