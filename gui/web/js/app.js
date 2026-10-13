/* ===========================================================================
   app.js —— Sonar 配置工具前端
   界面与 C++ 完全解耦：所有操作走 Backend.call(method, params)。
   在浏览器里打开也能跑（mock.js 提供假数据），方便调界面。
   =========================================================================== */
(function () {
'use strict';

const $  = (s, r) => (r || document).querySelector(s);
const $$ = (s, r) => [...(r || document).querySelectorAll(s)];

/* 中文按拼音排序。Chromium 的 ICU 原生支持 zh 拼音排序规则，
   所以不用像原 C++ 那样自己拿 LCMapStringW 生成排序键。 */
let COLL = null;
try { COLL = new Intl.Collator('zh-Hans-CN-u-co-pinyin', { numeric: true, sensitivity: 'base' }); }
catch (e) { try { COLL = new Intl.Collator('zh', { numeric: true }); } catch (e2) { COLL = null; } }
function cmpHan(a, b) {
  if (!a && !b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return COLL ? COLL.compare(a, b) : a.localeCompare(b);
}

/* 小工具：建 DOM */
function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  if (props) for (const k in props) {
    if (k === 'class') el.className = props[k];
    else if (k === 'html') el.innerHTML = props[k];
    else if (k === 'text') el.textContent = props[k];
    else if (k.startsWith('on') && typeof props[k] === 'function') el.addEventListener(k.slice(2), props[k]);
    else if (props[k] === true) el.setAttribute(k, '');
    else if (props[k] != null && props[k] !== false) el.setAttribute(k, props[k]);
  }
  for (const kid of kids.flat()) {
    if (kid == null || kid === false) continue;
    el.appendChild(typeof kid === 'string' || typeof kid === 'number'
      ? document.createTextNode(String(kid)) : kid);
  }
  return el;
}

function toast(msg, kind) {
  const t = h('div', { class: 'toast' + (kind ? ' ' + kind : ''), text: msg });
  $('#toasts').appendChild(t);
  setTimeout(() => { t.style.opacity = '0'; t.style.transition = 'opacity .3s'; }, 2400);
  setTimeout(() => t.remove(), 2800);
}

/* 极简 Markdown → HTML，只覆盖发布说明里实际用到的那几种语法。
   ★ 说明文本是从 GitHub 拉回来的**外部内容**，一律先转义再套标签，
     绝不能直接当 HTML 塞进 DOM。
   ★ URL 渲染成 <code> 而不是 <a>：这是 WebView，点链接会把整个界面导航走，
     要开网页请用窗口里的「打开发布页」按钮（那个会调系统浏览器）。 */
function notesToHtml(src) {
  const s = String(src).trim();
  // GitHub release 说明本身可能是 HTML（我们用 API 发的就是 <h3><ul><li>），
  // 那就直接渲染；否则按 Markdown 处理（老版本发布说明是 md）。
  if (/<(h[1-6]|ul|ol|li|p|br|b|strong|em|code|a|div)\b/i.test(s)) {
    // 只允许安全标签，去掉 script/style/on* 属性，避免注入
    return s
      .replace(/<\s*(script|style|iframe|object|embed)[^>]*>[\s\S]*?<\s*\/\s*\1\s*>/gi, '')
      .replace(/\son\w+\s*=\s*("[^"]*"|'[^']*'|[^\s>]+)/gi, '')
      .replace(/javascript:/gi, '');
  }
  return mdToHtml(s);
}

function mdToHtml(src) {
  const esc = s => String(s).replace(/[&<>"]/g,
    c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const inline = s => s
    .replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>')
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/&lt;(https?:\/\/[^&\s]+)&gt;/g, '<code>$1</code>');
  const out = [];
  let inList = false;
  const closeList = () => { if (inList) { out.push('</ul>'); inList = false; } };
  for (const raw of String(src || '').replace(/\r\n?/g, '\n').split('\n')) {
    const t = esc(raw.replace(/\s+$/, ''));
    if (!t.trim()) { closeList(); continue; }
    let m;
    if ((m = t.match(/^###\s+(.*)$/))) { closeList(); out.push('<h4>' + inline(m[1]) + '</h4>'); continue; }
    if ((m = t.match(/^##\s+(.*)$/)))  { closeList(); out.push('<h3>' + inline(m[1]) + '</h3>'); continue; }
    if ((m = t.match(/^#\s+(.*)$/)))   { closeList(); out.push('<h2>' + inline(m[1]) + '</h2>'); continue; }
    if ((m = t.match(/^\s*[-*]\s+(.*)$/))) {
      if (!inList) { out.push('<ul>'); inList = true; }
      out.push('<li>' + inline(m[1]) + '</li>');
      continue;
    }
    closeList();
    out.push('<p>' + inline(t) + '</p>');
  }
  closeList();
  return out.join('');
}

// ===========================================================================
//   彩蛋：点击左上角图标若干次，弹出"完成成就"文案（两行：成就名 + 描述）
// ===========================================================================
const EGG_ACHIEVEMENTS = [
  { t: '声呐学徒',       d: '你发现了这个会响的图标' },
  { t: '关注真夜中のloop喵~', d: '点击前往作者 B 站主页', url: 'https://space.bilibili.com/478832252' },
  { t: '打字机技师',     d: '再点几下，也许会有别的事发生' },
  { t: '猎人的直觉',     d: '第 7 次点击，声呐听见了你' },
  { t: '无意义的胜利',   d: '为没有意义的点击干杯' },
  { t: '声呐大师',       d: '听，那是猫猫在回应你' },
];
let eggClicks = 0;
let eggReady = true;
const EGG_FIRST_AT = 7;      // 第 7 次点击触发第一个
const EGG_EVERY = 10;        // 之后每 10 次再弹下一个
let eggNextAt = EGG_FIRST_AT;
let eggIndex = 0;

// 彩蛋②：耄耋哈气 —— 正常点击 10 次之后，之后每次点击有 5% 概率哈气
//（哈气时猫猫会抖一下 + 冒情绪气泡，和平时"喵"的反馈明显不同）
const EGG_HISS_AFTER = 10;
const EGG_HISS_CHANCE = 0.05;
const EGG_HISS_MS = 1250;      // 哈气持续时长 = 冷却时长：这段时间内点击无效
let eggHissing = false;

// 哈气：播 hiss.wav（内嵌资源），给徽标加抖动动画，并在旁边弹一个情绪气泡。
// 哈气期间进入冷却：点击一律忽略（等动画/气泡结束），避免连点把哈气刷掉。
function hissEgg() {
  if (eggHissing) return;
  eggHissing = true;
  Backend.call('ui.playAsset', { name: 'hiss.wav' }).catch(() => {});
  const badge = $('.logo-badge');
  if (!badge) { eggHissing = false; return; }
  badge.classList.remove('hiss');
  void badge.offsetWidth;          // 强制重排，让动画能连续触发
  badge.classList.add('hiss');
  const old = badge.querySelector('.hiss-bubble');
  if (old) old.remove();
  const bubble = h('div', { class: 'hiss-bubble', text: '😾 哈——！' });
  badge.appendChild(bubble);
  setTimeout(() => {
    badge.classList.remove('hiss');
    bubble.remove();
    eggHissing = false;            // 冷却结束，恢复可点击
  }, EGG_HISS_MS);
}

// 成就专属 toast：两行（第一行成就名、第二行描述；带链接的描述可点击跳转）
function eggToast(a) {
  // 成就弹出音效（coinmul.wav：内嵌在 exe 里，解压到 %LOCALAPPDATA% 后播放）
  Backend.call('ui.playAsset', { name: 'coinmul.wav' }).catch(() => {});
  const t = h('div', { class: 'toast egg' },
    h('div', { class: 'egg-line1' }, '🏆 完成成就：' + a.t),
    h('div', { class: 'egg-line2' },
      a.url
        ? h('button', {
            class: 'egg-link',
            onclick: () => {
              Backend.call('ui.openUrl', { url: a.url })
                .catch(e => toast('打不开链接：' + e.message, 'err'));
            },
          }, a.d)
        : document.createTextNode(a.d)));
  $('#toasts').appendChild(t);
  // 成就值得多看几秒：5 秒后消失
  setTimeout(() => { t.style.opacity = '0'; t.style.transition = 'opacity .3s'; }, 5000);
  setTimeout(() => t.remove(), 5400);
}

function bindLogoEgg() {
  const logo = $('#logoImg');
  if (!logo) return;
  logo.addEventListener('click', () => {
    // 哈气冷却期间点击无效（等哈气动画与气泡结束）
    if (eggHissing) return;
    // 点猫猫叫一声（内嵌的 meow.wav，解压后播放；连点会先停掉上一条再播）
    Backend.call('ui.playAsset', { name: 'meow.wav' }).catch(() => {});
    eggClicks++;
    // 耄耋哈气：正常点击 10 次之后，每次 5% 概率（与成就提示互不影响）
    if (eggClicks > EGG_HISS_AFTER && Math.random() < EGG_HISS_CHANCE) hissEgg();
    if (!eggReady || eggClicks < eggNextAt) return;
    eggReady = false;
    const a = EGG_ACHIEVEMENTS[eggIndex % EGG_ACHIEVEMENTS.length];
    eggIndex++;
    eggNextAt = eggClicks + EGG_EVERY;
    eggToast(a);
    // 冷却 4 秒，防止连点刷屏
    setTimeout(() => { eggReady = true; }, 4000);
  });
}

/* ===========================================================================
   通用确认框：删除这种破坏性操作一律走它
   =========================================================================== */
function confirmBox({ title, message, okText, danger, input, placeholder }) {
  return new Promise(resolve => {
    const m = $('#mConfirm');
    $('#cfTitle').textContent = title || '确认';
    $('#cfMsg').innerHTML = message || '';
    const inp = $('#cfInput');
    inp.hidden = !input;
    inp.value = '';
    if (input) inp.placeholder = placeholder || '';
    const ok = $('#cfOk'), cancel = $('#cfCancel');
    ok.textContent = okText || '确定';
    ok.classList.toggle('primary', !danger);
    ok.style.borderColor = danger ? 'var(--c-red)' : '';
    ok.style.background = danger ? 'var(--c-red)' : '';
    ok.style.color = danger ? '#fff' : '';
    function done(v) {
      m.classList.remove('open');
      ok.removeEventListener('click', onOk);
      cancel.removeEventListener('click', onCancel);
      inp.removeEventListener('keydown', onKey);
      resolve(v);
    }
    function onOk() { done(input ? (inp.value.trim() || false) : true); }
    function onCancel() { done(false); }
    function onKey(e) { if (e.key === 'Enter') onOk(); }
    ok.addEventListener('click', onOk);
    cancel.addEventListener('click', onCancel);
    inp.addEventListener('keydown', onKey);
    m.classList.add('open');
    setTimeout(() => (input ? inp : ok).focus(), 30);
  });
}

/* ===========================================================================
   应用状态
   =========================================================================== */
const ST = {
  ready: false,
  theme: 'clean-light',
  themes: [],
  wemView: false,          // 捕获面板视图：false=派生，true=WEM 音效
  assetBase: '../',
  iconBase: 'https://sonar.assets/weapons_icons/',   // 内嵌资源（exe 里解压出来），不在游戏目录
  logoUrl: 'https://sonar.assets/sonar_icon.png',
  weapons: [],
  anyCount: 0,
  entryCount: 0,
  judges: [],
  global: {},
  hotkeys: {},
  active: {},
  weaponFilter: -1,
  weaponOn: new Array(14).fill(false),   // 武器触发开关（默认全关）
  onlyActive: false,
  search: '',
  sort: { key: 'name', asc: true },
  entries: [],
  visible: [],          // 当前筛选+排序后的 [{entry, index}]
  selIndex: -1,
  combos: [],           // 当前武器可选组合
  live: { attached: false, history: [] },
  histExpanded: false,
  upd: { state: 0, progress: 0, tag: '', notes: '', err: '', needRestart: false },
};

const wName = id => {
  if (id < 0) return '任意';
  const w = ST.weapons.find(x => x.id === id);
  return w ? w.name : ('武器' + id);
};

/* 武器图标文件名 —— 与 weapons_icons/ 下的文件一一对应。
   ★ 后端只给武器 id：它返回的 icon 字段是 "w0"/"wany" 这种**编号**，不是文件名。
     文件名统一由前端定，将来换图/改扩展名不用动 C++。
   【踩过的坑】之前直接拿后端的 "w0" 去拼 URL，拼出 ../weapons_icons/w0，
     全是 404，整个武器列看起来就是一排"碎图"占位符。 */
const ICON_FILES = {
  0:  'great-sword1.png',
  1:  'sword-and-shield1.png',
  2:  'dual-blades1.png',
  3:  'long-sword1.png',
  4:  'hammer1.png',
  5:  'hunting-horn1.png',
  6:  'lance1.png',
  7:  'gunlance1.png',
  8:  'switch-axe1.png',
  9:  'charge-blade1.png',
  10: 'insect-glaive1.png',
  11: 'bow1.png',
  12: 'heavy-bowgun1.png',
  13: 'light-bowgun1.png',
};
const wIconUrl = id => (ICON_FILES[id] ? ST.iconBase + ICON_FILES[id] : null);

/* 图标加载失败时**不要留碎图占位符**：就地换成文字/符号，并在控制台留一行原因。
   这样即使哪天资源没部署全，界面也是"能看"的，而不是一排碎图。 */
function iconImg(url, fallbackText) {
  const img = h('img', { src: url, alt: '' });
  img.onerror = () => {
    console.warn('[Sonar] 武器图标加载失败:', url);
    const span = document.createElement('span');
    span.className = 'icon-fallback';
    span.textContent = fallbackText || '◈';
    if (img.parentNode) img.parentNode.replaceChild(span, img);
  };
  return img;
}

const wchip = (id, cls) => {
  const u = wIconUrl(id);
  const c = 'wchip' + (cls ? ' ' + cls : '');
  if (!u) return h('div', { class: c + ' all' }, '◈');
  return h('div', { class: c }, iconImg(u, '◈'));
};
const wInline = id => {
  const u = wIconUrl(id);
  if (!u) return h('span', { class: 'tw', style: 'color:var(--muted)' }, '任意');
  const box = h('span', { class: 'tw' });
  box.appendChild(iconImg(u, '◈'));
  box.appendChild(document.createTextNode(wName(id)));
  return box;
};

/* ===========================================================================
   主题
   =========================================================================== */
function applyTheme(id) {
  ST.theme = id;
  document.documentElement.setAttribute('data-theme', id);
  // 版本徽章右侧的主题名要跟着变，否则切了主题还显示旧的（用户看到一直是 clean-light）
  const v = $('#verText');
  if (v) {
    const m = (v.textContent || '').match(/^v[^\s·]*/);
    v.textContent = (m ? m[0] : 'v—') + ' · ' + id;
  }
  $$('#themePicker .theme-sw').forEach(b => b.classList.toggle('on', b.dataset.t === id));
}
function renderThemePicker() {
  const box = $('#themePicker');
  box.innerHTML = '';
  (ST.themes.length ? ST.themes : [
    { id: 'clean-light', name: '极简浅色' }, { id: 'pro-dark', name: '深色专业' },
    { id: 'cute-pastel', name: '可爱风' }, { id: 'sticker-outline', name: '描边贴纸' },
  ]).forEach(t => {
    const b = h('button', {
      class: 'theme-sw' + (t.id === ST.theme ? ' on' : ''), 'data-t': t.id, title: t.name + ' — ' + (t.desc || ''),
      onclick: async () => {
        applyTheme(t.id);
        try { await Backend.call('theme.set', { theme: t.id }); toast('界面风格：' + t.name, 'ok'); }
        catch (e) { toast('保存主题失败：' + e.message, 'err'); }
      },
    }, h('i'), h('i'));
    box.appendChild(b);
  });
}

/* ===========================================================================
   左栏：武器 + 组合
   =========================================================================== */
// 复制组合对话框：目标武器变化时重填组合下拉
async function fillCopyCombos() {
  const sel = $('#copyCombo');
  if (!sel) return;
  const tw = parseInt($('#copyWeapon').value, 10);
  sel.innerHTML = '';
  sel.appendChild(h('option', { value: '', text: '默认' }));
  try {
    const r = await Backend.call('combo.list', { weapon: tw });
    (r.combos || []).forEach(c => { if (c) sel.appendChild(h('option', { value: c, text: c })); });
  } catch (e) { /* 忽略：至少能选默认 */ }
}

function renderWeapons() {
  const box = $('#weaponList');
  box.innerHTML = '';
  const rows = [
    { id: -1, name: '全部条目', count: ST.entryCount },
    ...ST.weapons.map(w => ({ id: w.id, name: w.name, count: w.count, icon: w.icon })),
  ];
  if (ST.anyCount > 0) rows.push({ id: -2, name: '通用(任意)', count: ST.anyCount });

  rows.forEach(r => {
    // ★ 武器触发开关（0..13）：勾上才允许该武器的音效被触发（默认全关）
    let sw = null;
    if (r.id >= 0 && r.id < 14) {
      sw = h('input', {
        type: 'checkbox', class: 'wsw',
        checked: !!ST.weaponOn[r.id],
        title: '勾上 = 允许触发该武器的音效（默认全关）',
        onclick: ev => { ev.stopPropagation(); toggleWeaponOn(r.id); },
      });
    }
    const el = h('div', {
      class: 'wrow' + (ST.weaponFilter === r.id ? ' on' : ''),
      onclick: () => selectWeapon(r.id),
    }, sw, wchip(r.id === -1 ? -99 : r.id), h('span', { class: 'wname', text: r.name }),
       h('span', { class: 'wcount', text: String(r.count) }));
    box.appendChild(el);
  });
}

// 武器触发开关：默认全关；勾上才允许该武器的条目触发音效
function toggleWeaponOn(id) {
  if (id < 0 || id >= 14) return;
  ST.weaponOn[id] = !ST.weaponOn[id];
  Backend.call('weapons.enabled.set', { enabled: ST.weaponOn }).then(() => {
    toast((ST.weaponOn[id] ? '已启用：' : '已停用：') + weaponName(id), 'ok');
  }).catch(err => {
    ST.weaponOn[id] = !ST.weaponOn[id];
    toast('武器开关保存失败：' + ((err && err.message) || err), 'err');
    renderWeapons();
  });
  renderWeapons();
}
function weaponName(id) {
  const w = (ST.weapons || []).find(x => x.id === id);
  return w ? w.name : ('武器 ' + id);
}
async function loadWeaponOn() {
  try {
    const r = await Backend.call('weapons.enabled.get', {});
    ST.weaponOn = (r.enabled && r.enabled.length === 14) ? r.enabled.map(v => !!v) : new Array(14).fill(false);
  } catch { ST.weaponOn = new Array(14).fill(false); }
  renderWeapons();
}

async function selectWeapon(id) {
  ST.weaponFilter = id;
  ST.selIndex = -1;
  await loadCombos();
  renderWeapons();
  renderComboPanel();
  applyFilter();
  Backend.call('ui.set', { weaponFilter: id }).catch(() => {});
}

async function loadCombos() {
  const w = ST.weaponFilter;
  if (w < 0) { ST.combos = []; return; }
  try {
    const r = await Backend.call('combo.list', { weapon: w });
    ST.combos = r.combos && r.combos.length ? r.combos : [''];
    ST.active[w] = r.active || '';
  } catch { ST.combos = ['']; }
}

function renderComboPanel() {
  const panel = $('#comboPanel');
  const w = ST.weaponFilter;
  if (w < 0) { panel.hidden = true; return; }
  panel.hidden = false;
  const sel = $('#comboSelect');
  sel.innerHTML = '';
  ST.combos.forEach(c => sel.appendChild(
    h('option', { value: c, selected: (ST.active[w] || '') === c }, c === '' ? '默认' : c)));
  sel.onchange = async () => {
    await Backend.call('combo.setActive', { weapon: w, name: sel.value });
    ST.active[w] = sel.value;
    await refresh();
    // 切到别的组合后默认只看当前组合的条目（满屏"未激活"很吵），可随时取消勾选
    setOnlyActive(true);
    toast('已切换到组合：' + (sel.value || '默认'));
  };
}

// 只看激活组合的开关（切换组合后默认打开；也用于持久化）
function setOnlyActive(v) {
  ST.onlyActive = v;
  const cb = $('#onlyActive');
  if (cb) cb.checked = v;
  applyFilter();
  Backend.call('ui.set', { onlyActive: v }).catch(() => {});
}

/* ===========================================================================
   中栏：条目表（筛选 + 排序）
   =========================================================================== */
function entryActive(e) {
  if (e.weaponType < 0) return true;
  return (e.combo || '') === (ST.active[e.weaponType] || '');
}

function displayName(e) {
  if (e.name) return e.name;
  return e.fsmId >= 0 ? ('FSM ' + e.fsmId) : '条目';
}
// 条目里的全部音效：默认音效池 + 每个判定条件的音效池。
// ★ 判定音效也是"配了音效"，主列表/搜索都必须算上，
//   否则只配判定音效的条目会被当成"无音效"。
function entrySounds(e) {
  const out = [];
  (e.def || []).forEach(s => out.push(s));
  (e.conds || []).forEach(c => (c.pool || []).forEach(s => out.push(s)));
  return out;
}
function soundSummary(e) {
  const all = entrySounds(e);
  const first = all[0] || null;
  // ★ 只显示文件名：以前这里显示完整路径，长路径把「判定 / N 条」这些徽标挤出可见区
  //   （用户实测反馈"选了多少条、是否判定都看不见了"）。完整路径放 title 里。
  const base = p => {
    const s = String(p || '').replace(/\\/g, '/');
    const k = s.lastIndexOf('/');
    return k >= 0 ? s.slice(k + 1) : s;
  };
  const conds = (e.conds || []).length;
  const condSounds = (e.conds || []).reduce((n, c) => n + ((c.pool || []).length), 0);
  const gaugeSounds = (e.gauge || []).reduce((n, g) => n + ((g || []).length), 0);
  return {
    text: first ? base(first.path) : '(无音效)',
    fixed: !!(first && first.fixed),
    count: all.length,
    conds: conds,
    // 池明细（给 title 用）：默认 x 条 · 判定条件 y 个（z 条）· 刃时 w 条
    detail: `默认 ${(e.def || []).length} 条`
          + (conds ? ` · 判定条件 ${conds} 个（共 ${condSounds} 条）` : '')
          + (gaugeSounds ? ` · 刃时 ${gaugeSounds} 条` : ''),
  };
}
function firstLmt(e) { return e.lmtAny || !e.lmt || !e.lmt.length ? -1 : e.lmt[0]; }

function applyFilter() {
  const q = ST.search.trim().toLowerCase();
  let list = ST.entries.map((e, i) => ({ e, i }));

  // 武器筛选。
  // 【默认列出全部条目，不按激活组合隐藏】—— 原版只列激活组合的条目，
  // 结果条目一旦落到别的组合里，用户就以为"条目消失了"。现在默认全列，
  // 非激活的打个「未激活」标记；想要旧行为可以勾「只看激活组合」。
  if (ST.weaponFilter === -2) list = list.filter(x => x.e.weaponType < 0);
  else if (ST.weaponFilter >= 0) list = list.filter(x => x.e.weaponType === ST.weaponFilter);
  if (ST.onlyActive) list = list.filter(x => entryActive(x.e));
  // 视图级过滤（renderTable 里按 ST.wemView 决定显示派生还是 wem）

  // 搜索（音效路径含判定音效池，见 entrySounds）
  if (q) list = list.filter(x => {
    const e = x.e;
    const hay = [displayName(e), String(e.fsmId), (e.lmt || []).join(','),
                 entrySounds(e).map(s => s.path).join(' ')].join(' ').toLowerCase();
    return hay.includes(q);
  });

  // 排序（同键保持加入顺序 = stable）
  const { key, asc } = ST.sort;
  list.sort((A, B) => {
    const a = A.e, b = B.e;
    let c = 0;
    if (key === 'name') c = cmpHan(displayName(a), displayName(b));
    else if (key === 'weapon') c = a.weaponType - b.weaponType;
    else if (key === 'lmt') c = firstLmt(a) - firstLmt(b);
    else if (key === 'fsm') c = a.fsmId - b.fsmId;
    else if (key === 'media') c = (a.media || 0) - (b.media || 0);
    else if (key === 'seq') c = (a.seqNum || 0) - (b.seqNum || 0);
    if (c === 0) c = A.i - B.i;
    return asc ? c : -c;
  });
  ST.visible = list;
  renderTable();
}

function renderTable() {
  const tb = $('#entryBody');
  tb.innerHTML = '';
  const wem = ST.wemView;
  // 双表头切换：WEM 视图 → WEM id / 序号 列
  const tf = $('#theadFsm'), tw = $('#theadWem');
  if (tf) tf.hidden = wem;
  if (tw) tw.hidden = !wem;
  // 视图级过滤：完全分开（派生视图只显示 fsm 条目，WEM 视图只显示 wem 条目）
  const view = ST.visible.filter(({ e }) => wem ? (e.media != null && e.media > 0)
                                                : !(e.media != null && e.media > 0));
  const empty = $('#entryEmpty');

  if (!view.length) {
    empty.hidden = false;
    empty.textContent = ST.entries.length
      ? (wem ? '没有 WEM 条目 — 从右侧「WEM 音效」捕获历史点"＋ 添加"创建'
             : '没有派生条目（换个筛选或清空搜索）')
      : '还没有任何条目 — 点右上角「＋ 新增条目」开始';
  } else empty.hidden = true;

  // ★ 按"触发条件"分组：同一条件（武器+FSM+目标层+LMT）写了好几条时，
  //   用一个分组标题行把它们圈起来，并给组内行加底色 —— 用户一眼能看出"这几条是同一个条件"，
  //   避免重复/多余配置（实测用户会把同一条件写好几条，然后以为"有的条目不生效"）。
  const condKey = e => [e.weaponType, e.fsmId, e.fsmTarget,
                         (e.lmtAny || !(e.lmt || []).length) ? 'any' : (e.lmt || []).join(',')].join('|');
  const groups = [];
  const groupOf = {};
  view.forEach(item => {
    const k = condKey(item.e);
    if (!groupOf[k]) { groupOf[k] = { key: k, items: [] }; groups.push(groupOf[k]); }
    groupOf[k].items.push(item);
  });
  groups.forEach(g => {
    if (g.items.length > 1) {
      tb.appendChild(h('tr', { class: 'grp' },
        h('td', { colspan: '6' },
          h('span', { class: 'grptag', text: `同一触发条件 · ${g.items.length} 条` }),
          h('span', { class: 'dim', text: '（同一动作会同时播放这几条；想合并成一条可点工具栏「合并同条件」）' }))));
    }
    g.items.forEach(item => { renderEntryRow(tb, item, g.items.length > 1); });
  });
  function renderEntryRow(tb, item, inGroup) {
  const { e, i } = item;
    const sm = soundSummary(e);
    const inCombo = entryActive(e);
    const isWem = wem;
    const tr = h('tr', {
      class: (i === ST.selIndex ? 'sel ' : '') + (inCombo ? '' : 'inactive') + (inGroup ? ' grpmem' : ''),
      title: inCombo ? '' : '这条属于别的组合，当前不生效（切换组合后才会生效）',
    },
      h('td', {}, h('span', { class: 'nm', title: displayName(e), text: displayName(e) }),
        isWem ? h('span', { class: 'badge wem', text: 'wem', style: 'margin-left:7px' }) : null,
        inCombo ? null : h('span', { class: 'badge warn', text: '未激活', style: 'margin-left:7px' })),
      h('td', {}, wInline(e.weaponType)),
      // WEM 视图：WEM id + 序号（nbnk 内第几个）；派生视图：LMT + FSMId
      isWem
        ? h('td', { class: 'lmt' }, h('span', { class: 'mono', text: String(e.media) }))
        : h('td', { class: 'lmt' }, e.lmtAny || !(e.lmt || []).length ? '不限' : e.lmt.join(',')),
      isWem
        ? h('td', { class: 'fsm', style: 'color:var(--c-blue)', title: e.seqNum ? '' :
            '这个 media id 不在已收录的 nbnk 里，没有序号（它仍能正常触发）' },
            wemSeq(e) || '—')
        : h('td', { class: 'fsm' }, String(e.fsmId)),
      // ★ 徽标放最前（长文件名不再把它们挤出可见区）；完整路径与池明细都放 title
      h('td', { title: (sm.detail + '\n' + (entrySounds(e).map(s => s.path).join('\n') || '没有音效')) },
        h('span', { class: 'snd' },
          sm.count ? h('span', { class: 'badge cnt', text: sm.count + ' 条' }) : null,
          sm.conds ? h('span', { class: 'badge judge', text: '判定 ' + sm.conds }) : null,
          sm.fixed ? h('span', { class: 'badge', text: 'F' }) : null,
          h('span', { class: 'sndname', text: sm.text }))),
      h('td', {}, h('div', { class: 'acts' },
        h('button', { onclick: ev => { ev.stopPropagation(); openEditor(i); } }, '编辑'),
        h('button', { onclick: async ev => {
          ev.stopPropagation();
          await Backend.call('entries.duplicate', { index: i });
          await refresh(); toast('已复制条目');
        } }, '复制'),
        h('button', { class: 'del', onclick: ev => { ev.stopPropagation(); askDelete([i]); } }, '删除'))),
    );
    tr.addEventListener('click', () => {
      ST.selIndex = i; renderTable();
      openEditor(i);
    });
    tb.appendChild(tr);
  }

  // 表头排序指示
  $$('#entryTable th.sortable').forEach(th => {
    th.classList.remove('asc', 'desc');
    if (th.dataset.sort === ST.sort.key) th.classList.add(ST.sort.asc ? 'asc' : 'desc');
  });

  const inactive = view.filter(x => !entryActive(x.e)).length;
  $('#entryFoot').textContent =
    `显示 ${view.length} / 共 ${ST.entries.length} 条` +
    (ST.weaponFilter >= 0 ? `　·　当前组合：${ST.active[ST.weaponFilter] || '默认'}` : '') +
    (inactive && !ST.onlyActive ? `　·　其中 ${inactive} 条属于别的组合（未激活）` : '') +
    (ST.wemView ? `　·　WEM 条目（按媒体 id 触发）` : `　·　排序：${{ name: '名称', weapon: '武器', lmt: 'LMT', fsm: 'FSMId' }[ST.sort.key]}`) +
    (ST.wemView ? '' : (ST.sort.asc ? ' 升序' : ' 降序'));
}

// 条目 WEM 序号：优先真值表（core 按 media id 查 DIDX 给 seqNum）；
// 表里没有时退回从规范名解析（agent 的 name=bank/NN.wem 或 mediaids 的 bank/NN.ogg）
function wemSeq(e) {
  if (e.seqNum != null && e.seqNum > 0) return '第 ' + e.seqNum + ' 个';
  const nm = e.name || '';
  const m = nm.match(/^[^/\\]+\/(\d+)(?:\.[a-z0-9]+)?(?:\s*[（(][^（）()]*[）)])?\s*$/i)
         || nm.match(/\/(\d+)(?:\.[a-z0-9]+)?\s*$/i);
  return m ? ('第 ' + m[1] + ' 个') : '';
}
// 归属 nbnk：bank 字段（core 已按表补全）；老条目可从规范名兜底
function wemBank(e) {
  if (e.bank) return e.bank;
  const m = (e.name || '').match(/^([^/\\]+)\//);
  return m ? m[1] : '';
}

/* 删除二次确认 —— 明确列出要删什么，不可撤销 */
async function askDelete(indices) {
  const names = indices.map(i => displayName(ST.entries[i]));
  const one = names.length === 1;
  const list = names.slice(0, 8).map(n => '· ' + n).join('\n');
  const more = names.length > 8 ? `\n… 另外 ${names.length - 8} 条` : '';
  const ok = await confirmBox({
    title: one ? '删除条目' : `删除 ${names.length} 个条目`,
    message: `确定要删除${one ? '这个条目' : '这些条目'}吗？\n\n${list}${more}\n\n` +
             `<b>删除后不可撤销</b>（保存前仍可关掉 GUI 放弃改动）。`,
    okText: '删除', danger: true,
  });
  if (!ok) return;
  await Backend.call('entries.remove', { indices });
  ST.selIndex = -1;
  await refresh();
  toast(`已删除 ${names.length} 个条目`, 'ok');
}

/* ===========================================================================
   右栏：实时捕获
   =========================================================================== */
// 地图 id → 名字。id 分段来自 HunterPie Stage.cs（见 MHW_Memory_Map_Skill.md §11.5）：
//   0 主菜单 / 101-109 地图 / 201-203 竞技场 / 301-306 据点 / 501-506 房间·训练场·集会
// 101=古代树、108=永霜冻土、109=聚魔之地 是文档明确的；102-107 按图鉴顺序补，
// 若与实际不符，按实测改（进图后看地图 id，对照人在哪张图）。
const MAP_NAMES = {
  0: '主菜单',
  101: '古代树森林', 102: '大蚁冢荒地', 103: '陆珊瑚台地',
  104: '瘴气之谷', 105: '龙结晶之地',
  108: '永霜冻土', 109: '聚魔之地',
  201: '竞技场', 202: '竞技场', 203: '竞技场',
  301: '星辰据点', 302: '星辰据点', 303: '研究所',
  305: '月辰据点', 306: '月辰据点',
  501: '房间', 502: '房间', 503: '房间', 504: '训练场',
  505: '集会区域', 506: '房间',
};
// 和平区域（非狩猎场）：据点/房间/训练场/集会 —— 这些地方不该触发动作音效
const PEACE_ZONES = [301, 302, 303, 305, 306, 501, 502, 503, 504, 505, 506];
function sceneLabel(mapId) {
  if (mapId == null || mapId < 0) return '场景：未知';
  const name = MAP_NAMES[mapId];
  return '场景：' + (name || ('#' + mapId)) + '（地图 ' + mapId + '）';
}
function isPeaceZone(mapId) {
  return PEACE_ZONES.indexOf(mapId) >= 0;
}

function wemLabel(r) {
  // 标识 = 归属 nbnk · 第N个（表驱动：seqNum 来自 core 的 DIDX 真值表）
  const bank = r.bank || r.wemBank || '';
  const seq = (r.seqNum != null ? r.seqNum : r.wemSeqNum);
  if (seq != null && seq > 0)
    return (bank ? bank + ' · ' : '') + '第' + seq + '个';
  if (bank) return bank;
  return 'media ' + r.wemMedia;
}
// 显示名：用户命名 > 官方注释名（如"瓶子声"）> 捕获面板才回退到「bank · 第N个」
function wemDisplayName(r) {
  if (r.custom || r.wemNameCustom) return r.name || r.wemName || wemLabel(r);
  const note = r.note || r.wemNote;
  if (note) return note;
  return wemLabel(r);
}

function histFiltered(H) {
  // 按当前选中武器隔离：选了具体武器只显示该武器的捕获
  let out = H;
  if (ST.weaponFilter >= 0) out = out.filter(r => r.weapon === ST.weaponFilter);
  // 导入了 nbnk 且勾选"只看这个 bank"时，只显示该 bank 的事件
  if (bankFilterOn()) out = out.filter(r => (r.bank || '') === BANK.name);
  // ★ 双保险：连续同 fsm/lmt/weapon 的派生行，若相隔不到 3 秒就折叠掉后一条
  //   （core 侧也去重，但插件/网络抖动时仍可能出现重复，界面上必须干净）
  const out2 = [];
  for (const r of out) {
    const p = out2[out2.length - 1];
    if (p && r.kind === 0 && p.kind === 0 &&
        r.fsm === p.fsm && r.lmt === p.lmt && r.weapon === p.weapon &&
        (typeof r.ageMs === 'number' && typeof p.ageMs === 'number'
          ? Math.abs(p.ageMs - r.ageMs) < 3000 : false)) {
      continue;   // 折叠
    }
    out2.push(r);
  }
  return out2;
}

function setWemView(v) {
  // 捕获面板的视图切换由 capture.js 自己管（两个页面各有切换按钮）
  ST.wemView = !!v;
  if (typeof Capture !== 'undefined') Capture.view = v ? 'wem' : 'derived';
  // 中栏条目列表的同样式切换按钮
  const lf = $('#viewListFsm'), lw = $('#viewListWem');
  if (lf && lw) { lf.className = 'vs' + (v ? '' : ' on'); lw.className = 'vs' + (v ? ' on' : '' ); }
  renderTable();
}
function wireWemView() {
  // 捕获面板的派生/WEM 切换由 capture.js 自己渲染并绑定；这里只绑中栏条目列表的切换
  const lf = $('#viewListFsm'), lw = $('#viewListWem');
  if (lf && lw) { lf.onclick = () => setWemView(false); lw.onclick = () => setWemView(true); }
}

function renderLive() {
  // ★ 捕获面板已模块化：渲染交给 js/capture.js（两条独立列表、两个宿主、显式过滤、逐行可播）
  if (typeof Capture !== 'undefined') Capture.render();
}
/* 派生捕获 → 添加/编辑条目（按 fsm/lmt 触发） */
const overlayMode = () => document.documentElement.classList.contains('overlay-mode');   // 浮窗里不弹大编辑窗

async function captureToEntry(r) {
  const fsm = r.fsm;
  if (fsm == null || fsm < 0) { toast('这条记录没有 fsm，无法添加', 'err'); return; }
  const w = (r.weapon != null && r.weapon >= 0 && r.weapon <= 13) ? r.weapon : -1;
  const lmt = (r.lmt != null && r.lmt > 0) ? r.lmt : -1;

  // 已有条目：只有在**当前组合**里有才算"已有"（跟 core 的 added 判断一致）。
  const curCombo = w >= 0 ? (ST.active[w] || '') : '';
  const hit = ST.entries.findIndex(e =>
    e.weaponType === w && e.fsmId === fsm &&
    (e.combo || '') === (w < 0 ? '' : curCombo) &&
    (e.lmtAny || (Array.isArray(e.lmt) && (lmt < 0 || e.lmt.includes(lmt)))));
  if (hit >= 0) {
    const idx = ST.entries[hit].index != null ? ST.entries[hit].index : hit;
    if (overlayMode()) { toast('已有这个动作的条目，请回主界面编辑'); return; }
    toast('已有这个动作的条目，已打开编辑', 'ok');
    return;
  }

  const entry = {
    name: r.name || ('fsm ' + fsm), weaponType: w,
    combo: w >= 0 ? (ST.active[w] || '') : '',
    fsmId: fsm, fsmTarget: -1, group: '', stop: false,
    lmt: lmt > 0 ? [lmt] : [], lmtAny: lmt <= 0,
    def: [], gauge: [[], [], [], []],
    checkDelayMs: 0, checkTimeoutMs: 0, checkOffsetMs: 150, endOnAction: false,
    checkMode: 0, judgePreset: 0, conds: [],
  };
  try {
    const res = await Backend.call('entries.save', { index: null, entry });
    await refresh();
    if (overlayMode()) toast('已添加到 GUI 条目列表，请回主界面编辑'); else if (res && res.index != null) openEditor(res.index);
    toast('已添加条目（配好音效后记得保存）', 'ok');
  } catch (e) { toast('添加失败：' + e.message, 'err'); }
}

function wemInBank(r) {
  if (!BANK.path || !BANK.media || !BANK.media.length) return false;
  for (let i = 0; i < BANK.media.length; i++) if (BANK.media[i].id === r.wemMedia) return true;
  return false;
}
// 捕获行点击：条目页 = 加入条目；nbnk 制作页 = 播放这个 nbnk 里对应的 wem
function wemRowClick(r) {
  if (ST.bankPage) {
    if (!BANK.path) { toast('先在左边导入一个 nbnk', 'err'); return; }
    Backend.call('bank.play', { path: BANK.path, media: r.wemMedia })
      .then(() => toast('播放 media ' + r.wemMedia + (r.bank ? '（' + r.bank + '）' : ''), 'ok'))
      .catch(e => toast(e.message || '播放失败', 'err'));
    return;
  }
  addWemRecord(r);
}

async function addWemRecord(r) {
  const media = r.wemMedia;
  if (media == null || media <= 0) { toast('这条记录没有 media id，无法添加', 'err'); return; }
  const w = (r.weapon != null && r.weapon >= 0 && r.weapon <= 13) ? r.weapon : -1;
  const combo = w < 0 ? '' : (ST.active[w] || '');
  // 当前组合里已有同 media → 打开编辑（w<0 时找任意武器条目）
  const hit = ST.entries.findIndex(e =>
    e.media === media && (w < 0 || e.weaponType === w) && (e.combo || '') === combo);
  if (hit >= 0) {
    const idx = ST.entries[hit].index != null ? ST.entries[hit].index : hit;
    if (overlayMode()) { toast('已添加到 GUI 条目列表，请回主界面编辑'); return; }
    toast('当前组合已有这个 wem，已打开编辑', 'ok');
    return;
  }
  const entry = {
    name: wemSeq(r) || wemBank(r) || ('media ' + media), weaponType: w, combo,
    media: media, bank: r.bank || '', fsmId: -1, fsmTarget: -1, group: '', stop: false,
    lmt: [], lmtAny: false,   // 不限不勾选：wem 条目可以填 LMT 作为附加条件
    def: [], gauge: [[], [], [], []],
    checkDelayMs: 0, checkTimeoutMs: 0, checkOffsetMs: 150, endOnAction: false,
    checkMode: 0, judgePreset: 0, conds: [],
  };
  try {
    const res = await Backend.call('entries.save', { index: null, entry });
    await refresh();
    if (overlayMode()) toast('已添加到 GUI 条目列表，请回主界面编辑'); else if (res && res.index != null) openEditor(res.index);
    toast('已添加 wem 条目（触发方式 = media id，配好音效后记得保存）', 'ok');
  } catch (e) { toast('添加失败：' + e.message, 'err'); }
}

/* ===========================================================================
   音效替换（nbnk Mod，实验性）
   流程：导入 nbnk → 为某个 media 选音频 → 转 wem（ffmpeg+Wwise）→ 替换进 bank → 导出
   =========================================================================== */
const BANK = { path: '', name: '', media: [], reps: {}, histSaved: false };   // histSaved: 本次替换是否已写进记录   // reps: mediaId -> 音频路径
let playingMedia = 0;                                       // 正在 GUI 内播放的 media（0=无）

function bankRepCount() { return Object.keys(BANK.reps).length; }

function renderBank() {
  const info = $('#bankInfo');
  const tb = $('#bankBody2') || $('#bankBody');
  tb.innerHTML = '';
  if (!BANK.path) {
    info.textContent = '还没有导入 nbnk。点「导入 nbnk…」选一个 sound bank 文件。';
    $('#bankFoot2').textContent = '';
    return;
  }
  info.textContent = `已导入：${BANK.name}.nbnk（${BANK.media.length} 条 media）\n${BANK.path}\n` +
    `已选替换：${bankRepCount()} 条 · 导出目录：plugins\\WeaponSoundEnhance\\wemmod\\（可改）`;
  // ★ 已命名的（用户命名库里的）默认排前面，其次按 bank 内序号
  const sorted = BANK.media.slice().sort((a, b) => {
    const ca = a.custom ? 1 : 0, cb = b.custom ? 1 : 0;
    if (ca !== cb) return cb - ca;
    return (a.seqInBank || a.seq || 0) - (b.seqInBank || b.seq || 0);
  });
  const show = sorted.slice(0, 800);
  show.forEach(m => {
    const rep = BANK.reps[m.id] || '';
    tb.appendChild(h('tr', {},
      h('td', { class: 'lmt' }, String(m.seqInBank || m.seq)),
      h('td', {}, h('span', { class: 'nm', title: m.custom ? m.name : '', text: m.custom ? m.name : (m.note || '—') })),
      h('td', { class: 'fsm', style: 'color:var(--c-blue)' }, String(m.id)),
      h('td', { class: 'lmt' }, String(m.size)),
      h('td', { title: rep, style: 'max-width:220px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap' },
        rep ? rep.split(/[\\/]/).pop() : '—'),
      h('td', {}, h('div', { class: 'acts' },
        h('button', { title: '在 GUI 内直接播放这条 wem', onclick: async (ev) => {
          try {
            const r = await Backend.call('bank.play', { path: BANK.path, media: m.id });
            if (!r.played) toast('播放失败（解码器可能没就位）', 'err');
            else { playingMedia = m.id; toast(`播放中：media ${m.id}`, 'ok'); }
          } catch (e) { toast('播放失败：' + e.message, 'err'); }
        } }, '播放'),
        h('button', { title: '停止播放', onclick: async () => {
          try { await Backend.call('bank.stopPlay', {}); } catch (e) {}
          playingMedia = 0;
        } }, '停止'),
        rep ? h('button', { title: '试听你选的替换音频\n' + rep, onclick: async () => {
          try {
            const r = await Backend.call('audio.playFile', { path: rep });
            if (!r.played) toast('替换音播放失败', 'err');
          } catch (e) { toast('替换音播放失败：' + e.message, 'err'); }
        } }, '听替换音') : null,
        h('button', { title: '给这条 wem 起个名字（保存进用户名字库，下次自动读取）', onclick: () => openNameDialog(m.id, m.name) }, '命名'),
        h('button', { onclick: async () => {
          const r = await Backend.call('audio.pickMulti', {});
          if (r.cancelled || !r.files || !r.files.length) return;
          BANK.reps[m.id] = r.files[0];
          renderBank();
        } }, '选音频'),
        rep ? h('button', { class: 'del', onclick: () => { delete BANK.reps[m.id]; renderBank(); } }, '取消') : null))));
  });
  $('#bankFoot2').textContent =
    `${BANK.media.length} 条 media（显示前 ${show.length} 条）· 已选 ${bankRepCount()} 条替换`;
}


// 捕获面板按 bank 过滤（导入 bank 后可只看它的事件）
function bankFilterOn() {
  const cb = $('#bankOnlyThis');
  return !!(cb && cb.checked && BANK.name);
}

/* ===========================================================================
   编辑器
   =========================================================================== */
const ED = { open: false, index: null, draft: null, adv: false, judgeBox: false, poolOpen: {} };

const JUDGE_FALLBACK = [
  { i: 0, name: '不判定' },
  { i: 1, name: '造成伤害（命中/落空·任意武器）' },
  { i: 2, name: '太刀 · 登龙 命中/落空' },
  { i: 3, name: '太刀 · 大居 成功/失败' },
  { i: 4, name: '大剑 · 真蓄 命中/落空' },
  { i: 5, name: '自定义（自己写条件）' },
];

function blankEntry() {
  return { name: '', weaponType: ST.weaponFilter >= 0 ? ST.weaponFilter : -1, combo: '',
    fsmId: -1, fsmTarget: -1, group: '', stop: false, lmt: [], lmtAny: true,
    def: [], gauge: [[], [], [], []],
    checkDelayMs: 0, checkTimeoutMs: 0, checkOffsetMs: 150, endOnAction: false,
    checkMode: 0, judgePreset: 0, conds: [] };
}

async function openEditor(index) {
  let e;
  if (index == null) e = blankEntry();
  else {
    const r = await Backend.call('entries.get', { index });
    e = r.entry;
  }
  ED.open = true; ED.index = index == null ? null : index;
  ED.draft = e;
  // 有条件但没记预设（老配置）：下拉显示"自定义"，否则看起来像"不判定"（用户会以为设置丢了）
  if (!e.judgePreset && (e.conds || []).length) e.judgePreset = 5;
  ED.isWem = !!(e && e.media != null && e.media > 0);
  ED.adv = !!(e.group || e.stop || (e.fsmTarget >= 0) || (e.checkTimeoutMs > 0) ||
              (e.gauge || []).some(g => g && g.length));
  ED.judgeBox = ED.adv;
  $('#edTitle').textContent = index == null ? '新增条目' : '编辑条目';
  fillEditor();
  $('#editor').classList.add('open');
}

function fillEditor() {
  const e = ED.draft;
  $('#edName').value = e.name || '';

  // wem 条目：显示 nbnk 相关信息，隐藏 FSM 行（触发不依赖 fsm/lmt）
  const isWem = ED.isWem;
  const wi = $('#edWemInfo');
  if (isWem) {
    let seq = wemSeq(e);
    const bank = wemBank(e);
    const wname = ((ST.weapons || []).find(w => w.id === e.weaponType) || {}).name || ('武器 ' + e.weaponType);
    wi.hidden = false;
    wi.textContent = 'wem 条目：' + (seq || e.name || ('media ' + e.media)) +
      ' · 媒体 id ' + e.media +
      ' · 归属 nbnk：' + (bank || '（不在已收录的 nbnk 表内）') + ' · ' + wname +
      '　|　LMT / FSMId 是可选附加条件（留空只按 wem 触发）';
    // wem 条目保留 LMT 行（作为"附加条件"），隐藏 FSMId 行
    const row = $('#edRowFsm');
    if (row) row.style.display = 'none';
  } else {
    wi.hidden = true;
    const row = $('#edRowFsm');
    if (row) row.style.display = '';
  }

  $('#edFsm').value = e.fsmId;
  $('#edLmtAny').checked = !!e.lmtAny;
  $('#edLmt').value = (e.lmt || []).join(',');
  $('#edAdv').checked = ED.adv;
  $('#edAdvBox').hidden = !ED.adv;
  $('#edGroup').value = e.group || '';
  $('#edFsmTarget').value = e.fsmTarget;
  $('#edStop').checked = !!e.stop;

  // 武器下拉
  const ws = $('#edWeapon');
  ws.innerHTML = '';
  ws.appendChild(h('option', { value: '-1' }, '任意 (-1)'));
  ST.weapons.forEach(w => ws.appendChild(
    h('option', { value: String(w.id), selected: w.id === e.weaponType }, `${w.id} ${w.name}`)));
  ws.onchange = () => { e.weaponType = parseInt(ws.value, 10); e.combo = ''; fillComboSelect(); fillPools(); };

  // 判定下拉
  const js = $('#edJudge');
  js.innerHTML = '';
  (ST.judges.length ? ST.judges : JUDGE_FALLBACK).forEach(j => js.appendChild(
    h('option', { value: String(j.i), selected: j.i === (e.judgePreset || 0) }, j.name)));
  js.onchange = () => { e.judgePreset = parseInt(js.value, 10); judgeNote(); };

  $('#edAdvanced').checked = ED.judgeBox;
  $('#edJudgeBox').hidden = !ED.judgeBox;
  $('#edDelay').value = e.checkDelayMs;
  $('#edTimeout').value = e.checkTimeoutMs;
  $('#edOffset').value = e.checkOffsetMs;
  $('#edEndOnAction').checked = !!e.endOnAction;
  $('#edFinalMode').checked = e.checkMode === 1;

  // 事件（重新绑，避免重复）
  $('#edName').oninput = ev => e.name = ev.target.value;
  $('#edFsm').oninput = ev => e.fsmId = parseInt(ev.target.value, 10) || -1;
  $('#edLmtAny').onchange = ev => { e.lmtAny = ev.target.checked; if (e.lmtAny) { e.lmt = []; $('#edLmt').value = ''; } lmtHint(); };
  $('#edLmt').oninput = ev => {
    e.lmt = parseLmt(ev.target.value);
    // 填了 LMT 就自动取消"不限"（否则用户以为填了、实际是不限）
    if (e.lmt.length) { e.lmtAny = false; $('#edLmtAny').checked = false; }
    lmtHint();
  };
  $('#edAdv').onchange = ev => { ED.adv = ev.target.checked; $('#edAdvBox').hidden = !ED.adv; fillPools(); };
  $('#edGroup').oninput = ev => e.group = ev.target.value;
  $('#edFsmTarget').oninput = ev => e.fsmTarget = parseInt(ev.target.value, 10);
  $('#edStop').onchange = ev => e.stop = ev.target.checked;
  $('#edDelay').oninput = ev => e.checkDelayMs = parseInt(ev.target.value, 10) || 0;
  $('#edTimeout').oninput = ev => e.checkTimeoutMs = parseInt(ev.target.value, 10) || 0;
  $('#edOffset').oninput = ev => e.checkOffsetMs = parseInt(ev.target.value, 10) || 0;
  $('#edEndOnAction').onchange = ev => e.endOnAction = ev.target.checked;
  $('#edFinalMode').onchange = ev => e.checkMode = ev.target.checked ? 1 : 0;
  $('#edAdvanced').onchange = ev => { ED.judgeBox = ev.target.checked; $('#edJudgeBox').hidden = !ED.judgeBox; };

  fillComboSelect();
  lmtHint();
  judgeNote();
  fillPools();
  fillConds();
}

function lmtHint() {
  const e = ED.draft;
  const el = $('#edLmtHint');
  if (e.lmtAny) { el.textContent = '→ 不限 LMT：只要 FSMId 相同就会触发'; return; }
  const raw = $('#edLmt').value.trim();
  if (!raw) { el.textContent = '→ 空 = 不限 LMT'; return; }
  const bad = raw.split(/[,;\s]+/).filter(s => s && !/^-?\d+$/.test(s));
  if (bad.length) { el.innerHTML = ''; el.append('→ 无法识别：', h('b', { text: bad.join(', ') }),
    '　只能填整数（逗号分隔）'); return; }
  el.textContent = '→ 只匹配 LMT：' + e.lmt.join(', ');
}
function parseLmt(s) {
  return s.split(/[,;\s]+/).map(t => t.trim()).filter(t => /^-?\d+$/.test(t)).map(Number);
}

function fillComboSelect() {
  const e = ED.draft;
  const sel = $('#edCombo');
  sel.innerHTML = '';
  sel.appendChild(h('option', { value: '' }, '默认组合'));
  if (e.weaponType >= 0) {
    Backend.call('combo.list', { weapon: e.weaponType }).then(r => {
      (r.combos || []).filter(c => c).forEach(c => sel.appendChild(
        h('option', { value: c, selected: c === e.combo }, c)));
    }).catch(() => {});
  }
  sel.onchange = () => e.combo = sel.value;
}

/* ===========================================================================
   判定预设：把"造成伤害 / 登龙 / 大居 / 真蓄"这些常用判定，一键填成
   **真正的条件 + 判定窗口参数**。

   为什么需要它：以前这个下拉只改 judgePreset 并显示一句提示，**不会生成任何条件**，
   于是"选了造成伤害"等于没选（没有条件 → 后端还会把判定窗口清零 → 变成普通条目），
   用户配完发现"判定没生效"。
   =========================================================================== */
const JUDGE_PRESET_DEFS = {
  // 通用：打出伤害就播命中，没打中在窗口结束时播落空
  1: { delay: 0, timeout: 2500, offset: 150, endOnAction: true, checkMode: 0,
       conds: [ { expr: 'dmg>0', atEnd: false, hint: '命中' },
                { expr: 'dmg==0', atEnd: true, hint: '落空' } ] },
  // 太刀 · 登龙：伤害在落地那一下，窗口给宽一点
  2: { delay: 0, timeout: 3000, offset: 150, endOnAction: true, checkMode: 0,
       conds: [ { expr: 'dmg>0', atEnd: false, hint: '命中' },
                { expr: 'dmg==0', atEnd: true, hint: '落空' } ] },
  // 太刀 · 大居：成功 = 打出伤害且不掉刃（可能被推翻，所以窗口结束才评）
  3: { delay: 0, timeout: 3000, offset: 150, endOnAction: true, checkMode: 1,
       conds: [ { expr: 'dmg>0 & dAura>=0', atEnd: true, hint: '成功' },
                { expr: 'dAura<0 | dmg==0', atEnd: true, hint: '失败' } ] },
  // 大剑 · 真蓄：两段伤害，判定起点卡在中间（1200ms）
  4: { delay: 1200, timeout: 3500, offset: 150, endOnAction: true, checkMode: 0,
       conds: [ { expr: 'dmg>0', atEnd: false, hint: '命中' },
                { expr: 'dmg==0', atEnd: true, hint: '落空' } ] },
};

// 按预设填充条件与窗口参数（会重写现有条件，调用前先让用户确认）
function applyJudgePreset() {
  const e = ED.draft;
  const n = e.judgePreset || 0;
  const d = JUDGE_PRESET_DEFS[n];
  if (!d) {
    toast(n === 5 ? '「自定义」不需要填充：请自己点「＋ 添加条件」写表达式'
                  : '「不判定」没有预设：动作一匹配就播', 'ok');
    return;
  }
  e.checkDelayMs = d.delay;
  e.checkTimeoutMs = d.timeout;
  e.checkOffsetMs = d.offset;
  e.endOnAction = d.endOnAction;
  e.checkMode = d.checkMode;
  // 保留同名表达式上已有的音效（避免用户配好的音频被清掉）
  const old = {};
  (e.conds || []).forEach(c => { old[c.expr] = c.pool || []; });
  e.conds = d.conds.map(c => ({
    expr: c.expr,
    atEnd: !!c.atEnd,
    pool: old[c.expr] || [],
  }));
  ED.judgeBox = true;
  $('#edAdvanced').checked = true;
  $('#edJudgeBox').hidden = false;
  $('#edDelay').value = e.checkDelayMs;
  $('#edTimeout').value = e.checkTimeoutMs;
  $('#edOffset').value = e.checkOffsetMs;
  $('#edEndOnAction').checked = !!e.endOnAction;
  $('#edFinalMode').checked = e.checkMode === 1;
  fillConds();
  judgeNote();
  toast(`已按预设填好 ${e.conds.length} 条条件：请给每条条件「浏览… 选音效」加上音效（没加音效的条件会被丢弃）`, 'ok');
}

// 判定区提示：预设说明 + 场景提醒（伤害类条件需要任务/目标怪物）
function judgeSceneWarn() {
  const L = ST.live || {};
  // 只在"已连接且已进场景"时才谈信号可用性；游戏没开时别乱报
  if (!L.attached || !L.inScene) return '';
  const sig = L.dmgSignal;   // 插件上报：quest=任务伤害可用 / hp=怪物血量可用 / none=都读不到
  if (sig === 'none') {
    const why = (L.mapId === 504) ? '训练场里木桩不算怪物、也没有任务累计伤害'
      : (L.questState !== 2 ? '当前不在任务中' : '没有可读的目标怪物');
    return '⚠ 伤害信号：读不到（' + why + '）—— dmg>0 这类条件不会成立，' +
           '判定会在窗口结束时走「默认音效」。请进任务、锁定目标怪物后再测。';
  }
  if (sig === 'quest') return '伤害信号：任务累计伤害 ✓（判定可用）';
  if (sig === 'hp')    return '伤害信号：怪物血量 ✓（判定可用；任务伤害读不到时用血量兜底）';
  return '';
}
function judgeNote() {
  const j = ED.draft.judgePreset || 0;
  const notes = {
    1: '打出伤害就播【命中】的音效，没打中播【落空】。不限定武器和动作。',
    2: '在登龙命中或落空时分别播放不同的 wav。',
    3: '大居成功需要打出伤害且不掉刃，所以在窗口结束时判定。',
    4: '真蓄两段：第一段约 0.65 秒、伤害小，第二段约 1.7~2.1 秒、伤害大。判定起点设在 1200ms 卡在两段中间。',
  };
  const warn = judgeSceneWarn();
  $('#edJudgeNote').textContent = (j >= 1 && j <= 4 ? notes[j]
    : (j === 5 ? '自己写条件表达式，例如 dmg>0 & dAura>=0' : '动作一匹配就播，不做判定。'))
    + (j >= 1 && j <= 4 ? '（要真正生效，请点右侧「按预设填充条件」）' : '')
    + (warn ? '\n' + warn : '');
}

/* ---- 音效卡片（普通池 / 刃时池 / 判定池共用同一套控件）---- */
function soundCard(sp, onRemove, onChange) {
  const wrap = h('div', { class: 'card2' });

  const pathRow = h('div', { class: 'path' }, sp.path || '(未选文件)');
  if (sp.fixed) pathRow.appendChild(h('span', { class: 'badge', text: 'F 固定', style: 'margin-left:7px' }));
  wrap.appendChild(pathRow);

  const mk = (label, get, set, min, max, step, w) => {
    const num = h('input', { type: 'number', class: 'mini', value: String(get()), min: String(min), max: String(max) });
    const rng = h('input', { type: 'range', min: String(min), max: String(max), value: String(get()),
      style: 'width:' + (w || 90) + 'px' });
    paintRange(rng);   // ★ 初始化就按真实值画填充，别让滑块停在"一半蓝一半灰"
    let syncing = false;
    const push = v => {
      if (syncing) return;
      syncing = true;
      v = Math.max(min, Math.min(max, v | 0));
      set(v); num.value = String(v); rng.value = String(v);
      paintRange(rng);   // ★ 数值变了必须重画
      syncing = false; onChange && onChange();
    };
    rng.oninput = () => push(parseInt(rng.value, 10));
    num.oninput = () => push(parseInt(num.value, 10) || 0);
    return h('div', { class: 'row' }, label, rng, num, h('em', {}, step));
  };

  wrap.appendChild(mk('延时', () => sp.delay, v => sp.delay = v, 0, 5000, 'ms', 110));
  wrap.appendChild(mk('音量', () => sp.vol, v => sp.vol = v, 0, 100, '', 80));
  wrap.appendChild(mk('冷却', () => sp.cdMs, v => sp.cdMs = v, 0, 60000, 'ms', 80));

  const fixedCb = h('input', { type: 'checkbox', checked: !!sp.fixed });
  fixedCb.onchange = () => { sp.fixed = fixedCb.checked; onChange && onChange(); };
  const lockCb = h('input', { type: 'checkbox', checked: !!sp.playLock });
  lockCb.onchange = () => { sp.playLock = lockCb.checked; onChange && onChange(); };

  wrap.appendChild(h('div', { class: 'row' },
    h('label', { class: 'inline' }, fixedCb, '固定(F)：命中时总是播放这条'),
    h('label', { class: 'inline' }, lockCb, '播放期间不重复')));
  wrap.appendChild(h('div', { class: 'row' },
    h('button', { onclick: () => Backend.call('sound.preview', { path: sp.path, vol: sp.vol, delay: sp.delay })
      .catch(e => toast('试听失败：' + e.message, 'err')) }, '试听'),
    h('button', { onclick: async () => {
      const r = await Backend.call('sound.pick', {});
      if (r && r.path) { sp.path = r.path; onChange && onChange(); refreshSoundCards(); }
    } }, '换'),
    h('button', { class: 'del', onclick: () => { onRemove(); } }, '移除')));
  return wrap;
}

/* ---- 音效池 ---- */
let POOL_REBUILD = null;
function refreshSoundCards() {
  if (POOL_REBUILD) POOL_REBUILD();
}
function fillPools() {
  const box = $('#edPools');
  const e = ED.draft;
  const rebuild = () => { box.innerHTML = ''; buildPools(box, e); };
  POOL_REBUILD = rebuild;
  rebuild();
}

// ★ 池配色引用主题变量（每个主题各自定义 --accent / --c-*）→ 自动跟随主题，不再固定蓝色
const POOL_ACCENTS = ['var(--accent)', 'var(--c-purple)', 'var(--c-amber)', 'var(--c-green)', 'var(--c-red)', 'var(--c-blue)'];
let poolSeq = 0;
const poolAccent = k => POOL_ACCENTS[Math.abs(k) % POOL_ACCENTS.length];

function buildPools(box, e) {
  const isLS = e.weaponType === 3;
  poolSeq = 0;   // 每次渲染重新分配配色
  e.gauge = e.gauge || [[], [], [], []];

  const pool = (title, arr, key, canRemove) => {
    const openKey = key;
    const id = key;
    const open = ED.poolOpen[id] !== false;
    // ★ 每个音效池各自成一个框（不同池不同配色）：用户要能一眼分清
    //   "这 5 条属于默认音效"、"这 3 条属于 dmg>0"、"那 2 条属于 dmg<0"。
    const wrap = h('div', { class: 'poolbox', style: `--pa:${poolAccent(poolSeq++)}` });
    const head = h('div', { class: 'poolhead' + (open ? '' : ' closed') },
      h('span', { class: 'arrow', text: '▼' }), `${title}  ${arr.length} 条`);
    head.onclick = () => { ED.poolOpen[id] = !open; fillPools(); };
    wrap.appendChild(head);
    box.appendChild(wrap);
    if (!open) return;

    arr.forEach((sp, i) => {
      wrap.appendChild(soundCard(sp, () => { arr.splice(i, 1); fillPools(); }, () => {}));
    });
    if (!arr.length) wrap.appendChild(h('div', { class: 'note', style: 'margin-bottom:8px' }, '(空)'));

    // 只保留"浏览…"选文件，不手填路径（手填容易填错，选文件最稳）
    const browse = h('button', { onclick: async () => {
      const r = await Backend.call('sound.pickMulti', {});
      (r.paths || []).forEach(p => {
        if (!arr.some(s => s.path === p))
          arr.push({ path: p, delay: 0, vol: 100, fixed: false, cdMs: 0, playLock: false });
      });
      fillPools();
    } }, '浏览… 选音效');
    wrap.appendChild(h('div', { class: 'addrow' }, browse));
  };

  $('#edDefHead').textContent = isLS ? '默认音效（任意刃时）' : '默认音效';
  pool('默认音效', e.def, 'def', true);

  if (isLS && ED.adv) {
    box.appendChild(h('div', { class: 'sect' }, '刃时音效（太刀专用，未配置的刃色回退默认音效）'));
    ['无刃时', '白刃时', '黄刃时', '红刃时'].forEach((nm, i) => {
      pool(nm, e.gauge[i], 'g' + i, true);
    });
  }
}

/* ---- 判定条件 ---- */
function fillConds() {
  let condSeq = 0;
  const box = $('#edConds');
  box.innerHTML = '';
  const e = ED.draft;
  e.conds = e.conds || [];
  e.conds.forEach((c, i) => {
    const expr = h('input', { type: 'text', value: c.expr || '', placeholder: '如 dmg>0 & dAura>=0',
      style: 'flex:1;min-width:220px' });
    expr.oninput = () => c.expr = expr.value;
    const atEnd = h('input', { type: 'checkbox', checked: !!c.atEnd });
    atEnd.onchange = () => c.atEnd = atEnd.checked;

    const del = h('button', { class: 'del', onclick: async () => {
      const ok = await confirmBox({ title: '删除条件', danger: true, okText: '删除',
        message: `确定删除条件「${c.expr || '(空)'}」吗？\n它的音效池（${(c.pool || []).length} 条）会一起删掉。` });
      if (!ok) return;
      e.conds.splice(i, 1); fillConds();
    } }, '删除');

    const cb = h('div', { class: 'condbox' },
      h('div', { class: 'row' }, h('span', { style: 'color:var(--muted);font-size:12px' }, '条件'), expr, del),
      h('div', { class: 'row' },
        h('label', { class: 'inline' }, atEnd, '只在窗口结束时判定（SoundEnd:）'),
        h('span', { class: 'tip' }, '变量：dmg / aura / dAura / charge / dCharge / lmt / fsm / fsmTarget / ms')));
    c.pool = c.pool || [];
    // ★ 这个条件的音效单独一个框（配色与该条件绑定），与其它条件/默认音效区分开
    const cwrap = h('div', { class: 'poolbox sub', style: `--pa:${poolAccent(condSeq++)}` },
      h('div', { class: 'poolhead' }, `条件音效池  ${c.pool.length} 条`));
    c.pool.forEach((sp, k) => cwrap.appendChild(
      soundCard(sp, () => { c.pool.splice(k, 1); fillConds(); }, () => {})));
    cwrap.appendChild(h('div', { class: 'addrow' },
      h('button', { onclick: async () => {
        const r = await Backend.call('sound.pickMulti', {});
        (r.paths || []).forEach(p => {
          if (!c.pool.some(s => s.path === p))
            c.pool.push({ path: p, delay: 0, vol: 100, fixed: false, cdMs: 0, playLock: false });
        });
        fillConds();
      } }, '浏览… 选音效')));
    cb.appendChild(cwrap);
    box.appendChild(cb);
  });
  if (!e.conds.length) box.appendChild(h('div', { class: 'note' }, '还没有条件：点下面「＋ 添加条件」'));
}

async function saveEditor() {
  const e = ED.draft;
  // ★ 选了判定模式预设但还没有条件 → 保存时自动按预设生成（否则窗口会被清空、
  //   重新打开时下拉又变回"不判定"，用户看到的就是"选了没用"）
  if (e.judgePreset >= 1 && e.judgePreset <= 4 && !(e.conds || []).length) {
    applyJudgePreset();
    toast('已按判定模式预设自动生成判定条件：请给每条条件加音效（没加音效的条件会被丢弃）', 'ok');
  }
  // ★ 条件没有音效会被后端丢弃（空音效池的条件没有意义）→ 先提醒，别让用户白配
  const emptyConds = (e.conds || []).filter(c => !(c.pool && c.pool.length));
  if (emptyConds.length) {
    const names = emptyConds.map(c => c.expr || '(空表达式)').join('、');
    const go = await confirmBox({
      title: '有条件的音效是空的', danger: true, okText: '仍然保存',
      message: `这些条件还没有加音效：${names}\n保存后它们会被丢弃（判定窗口也会跟着失效）。\n给条件加音效：展开「判定时间与条件」→ 条件下面点「浏览… 选音效」。`,
    });
    if (!go) return;
  }
  if (!e.name) e.name = (e.fsmId >= 0 ? 'FSM ' + e.fsmId : '条目');
  if (e.weaponType < 0) e.combo = '';
  try {
    await Backend.call('entries.save', { index: ED.index, entry: e });
    $('#editor').classList.remove('open');
    ED.open = false;
    await refresh();
    toast(ED.index == null ? '已新增条目' : '已修改条目（记得保存）', 'ok');
  } catch (err) { toast('保存失败：' + err.message, 'err'); }
}

/* ===========================================================================
   模态：FSM 查询 / ID 库 / 更新
   =========================================================================== */
function openModal(id) { $(id).classList.add('open'); syncModalOpenClass(); if (id === '#mUpd') refreshUpdate(); }
function closeModals() { $$('.modal').forEach(m => { if (m.id !== 'mConfirm') m.classList.remove('open'); }); syncModalOpenClass(); }

// ★ 弹窗开着时给 <body> 挂 modal-open —— CSS 借此隐藏主题装饰层
//   （可爱风的巨型圆角装饰会盖在弹窗内容上）
function syncModalOpenClass() {
  document.body.classList.toggle('modal-open', !!document.querySelector('.modal.open'));
}

async function runFsmSearch() {
  const q = $('#fsmQ').value;
  const w = parseInt($('#fsmW').value, 10);
  try {
    const r = await Backend.call('fsm.search', { q, weapon: w });
    const tb = $('#fsmBody'); tb.innerHTML = '';
    (r.rows || []).forEach(row => {
      const b = h('button', {
        class: 'mini ok',
        style: 'font-size:11px;padding:2px 8px;white-space:nowrap',
        title: '把这个动作加进当前激活组合',
        onclick: () => addFsmToCombo(row),
      }, '＋ 加入');
      const st = r.added ? '已添加' : '';
      tb.appendChild(h('tr', {},
        h('td', {}, row.name || ''),
        h('td', {}, wInline(row.weapon)),
        h('td', { class: 'lmt' }, String(row.lmt)),
        h('td', { class: 'fsm' }, String(row.fsm)),
        h('td', {}, b, st ? h('span', { class: 'badge', text: '已添加', style: 'margin-left:6px' }) : null)));
    });
    $('#fsmFoot').textContent = `共 ${r.total || 0} 条`;
  } catch (e) { toast('查询失败：' + e.message, 'err'); }
}

// FSM 查询里一键加入当前组合：该动作在当前组合已配过 → 打开编辑；否则新建条目
async function addFsmToCombo(row) {
  if (row.weapon == null || row.weapon < 0 || row.weapon > 13) {
    toast('这条动作没有固定的武器，无法加入组合', 'err'); return;
  }
  if (row.fsm == null || row.fsm < 0) { toast('这条动作没有 fsm，无法加入', 'err'); return; }
  const combo = ST.active[row.weapon] || '';
  const lmt = (row.lmt != null && row.lmt > 0) ? row.lmt : -1;

  // 当前组合里已有同 fsm 的条目 → 打开编辑
  const hit = ST.entries.findIndex(e =>
    e.weaponType === row.weapon && (e.combo || '') === combo && e.fsmId === row.fsm &&
    (e.lmtAny || (Array.isArray(e.lmt) && (lmt < 0 || e.lmt.includes(lmt)))));
  if (hit >= 0) {
    const idx = ST.entries[hit].index != null ? ST.entries[hit].index : hit;
    openEditor(idx);
    toast('当前组合已有这个动作，已打开编辑', 'ok');
    return;
  }

  const entry = {
    name: row.name || ('fsm ' + row.fsm), weaponType: row.weapon, combo,
    fsmId: row.fsm, fsmTarget: -1, group: '', stop: false,
    lmt: lmt > 0 ? [lmt] : [], lmtAny: lmt <= 0,
    def: [], gauge: [[], [], [], []],
    checkDelayMs: 0, checkTimeoutMs: 0, checkOffsetMs: 150, endOnAction: false,
    checkMode: 0, judgePreset: 0, conds: [],
  };
  try {
    const res = await Backend.call('entries.save', { index: null, entry });
    await refresh();
    if (res && res.index != null) openEditor(res.index);
    toast(`已加入「${combo || '默认'}」组合（配好音效后记得保存）`, 'ok');
  } catch (e) { toast('加入失败：' + e.message, 'err'); }
}

async function refreshIds() {
  $('#idsPaths').textContent = '基础库：fsm_db.csv（随版本更新，只追加行）\n用户库：fsm_db_user.csv（你的实测/导入都进这里）';
  try {
    const r = await Backend.call('fsm.captured', {});
    const tb = $('#idsBody'); tb.innerHTML = '';
    (r.captured || []).forEach(c => tb.appendChild(h('tr', {},
      h('td', {}, c.name || ''), h('td', {}, wInline(c.weapon)),
      h('td', { class: 'lmt' }, String(c.lmt)), h('td', { class: 'fsm' }, String(c.fsm)),
      h('td', {}, c.added ? h('span', { class: 'badge', text: '已添加' }) : ''))));
  } catch {}
}

/* ===========================================================================
   热键与聊天指令
   键值就是 Windows 虚拟键码（VK_*），和浏览器 keydown 的 keyCode 是同一套，
   所以"点一下格子、再按一个新键"就能直接改绑，不需要手填数字。
   =========================================================================== */
const VK_NAMES = {
  8:'Backspace', 9:'Tab', 13:'Enter', 16:'Shift', 17:'Ctrl', 18:'Alt', 19:'Pause',
  20:'CapsLock', 27:'Esc', 32:'空格', 33:'PageUp', 34:'PageDown', 35:'End', 36:'Home',
  37:'←', 38:'↑', 39:'→', 40:'↓', 45:'Insert', 46:'Delete',
  144:'NumLock', 145:'ScrollLock',
};
const VK_SYMS = {186:';', 187:'=', 188:',', 189:'-', 190:'.', 191:'/', 192:'`',
                 219:'[', 220:'\\', 221:']', 222:"'"};
function vkName(code) {
  if (code == null) return '未设置';
  if (code === 0) return '（关闭）';
  if (VK_NAMES[code]) return VK_NAMES[code];
  if (code >= 48 && code <= 57) return '主键盘 ' + String.fromCharCode(code);
  if (code >= 65 && code <= 90) return String.fromCharCode(code);
  if (code >= 96 && code <= 105) return '小键盘 ' + (code - 96);
  if (code >= 112 && code <= 123) return 'F' + (code - 111);
  if (VK_SYMS[code]) return VK_SYMS[code];
  return '键码 ' + code;
}

// 从 core 读写的字段名 → 界面上的用途说明
const HOTKEYS = [
  { key: 'reloadKey',   label: '重载配置' },
  { key: 'volUpKey',    label: '音量 +5' },
  { key: 'volDownKey',  label: '音量 −5' },
  { key: 'setVolKey',   label: '设为指定音量' },
  { key: 'toggleKey',   label: '总开关 开 / 关' },
  { key: 'moreKey',     label: '更多音效 开 / 关' },
  { key: 'comboKey',    label: '切换配置组合' },
];

// 指令表：内容与插件 WeaponSoundEnhance.cpp 里的 /wse 分支一一对应
const CHAT_CMDS = [
  ['/wse', '查看状态'],
  ['/wse reload', '重新读取 ini，改动立刻生效（等价工具栏「重载游戏配置」）'],
  ['/wse on  /  off', '总开关 开 / 关'],
  ['/wse more  /  one', '更多音效 开 / 只播一条'],
  ['/wse stop', '停掉正在播放的音效'],
  ['/wse vol N', '音量设为 N（0 – 100）'],
  ['/wse vol+  /  vol-', '音量 +5 / −5'],
  ['/wse combo', '切到当前武器的下一个配置组合'],
  ['/wse combo 名字', '切到指定的配置组合'],
  ['/wse combos', '列出该武器所有组合，当前那个用 [ ] 标出'],
];

let HK = {};             // 当前热键值（字段名 → VK 码）
let hkCapture = null;    // 正在等待改绑的字段名

function renderHotkeys() {
  const tb = $('#hkBody');
  tb.innerHTML = '';
  // ★ 回调参数不能叫 h —— 那会遮蔽全局的 DOM 构建函数 h()，
  //   于是 h('button',…) 变成"把对象当函数调用"，整张表一行都建不出来。
  HOTKEYS.forEach(item => {
    const btn = h('button', {
      class: 'keybtn' + (hkCapture === item.key ? ' capturing' : ''),
      title: '点这里，然后按一个新键',
      onclick: () => { hkCapture = item.key; renderHotkeys(); },
    }, hkCapture === item.key ? '按下新键…' : vkName(HK[item.key]));
    tb.appendChild(h('tr', {},
      h('td', {}, item.label),
      h('td', {}, btn),
      h('td', {}, h('button', {
        title: '把这个热键关掉',
        onclick: () => setHotkey(item.key, 0),
      }, '清除'))));
  });
}

function renderChatCmds() {
  const tb = $('#cmdBody');
  tb.innerHTML = '';
  CHAT_CMDS.forEach(([c, d]) => tb.appendChild(h('tr', {},
    h('td', {}, h('code', { text: c })),
    h('td', {}, d))));
}

async function refreshHotkeys() {
  HK = ST.hotkeys || {};
  const g = ST.global || {};
  $('#hkMod').value = String(HK.modifierKey == null ? 17 : HK.modifierKey);
  $('#hkVolVal').value = String(HK.setVolValue == null ? 50 : HK.setVolValue);
  $('#gChatCmd').checked = !!g.chatCommands;
  $('#gChatEcho').checked = !!g.chatEcho;
  hkCapture = null;
  renderHotkeys();
}

async function setHotkey(key, code) {
  const label = (HOTKEYS.find(item => item.key === key) || {}).label || key;
  hkCapture = null;
  try {
    const r = await Backend.call('hotkeys.set', { key, value: code });
    HK = r || HK;
    renderHotkeys();
    // modifierKey / setVolValue 不在 HOTKEYS 表里，名字单独给
    const shown = key === 'modifierKey' ? ({ 0: '不用组合键', 16: 'Shift', 17: 'Ctrl', 18: 'Alt' }[code] || vkName(code))
                : key === 'setVolValue' ? (code + '%')
                : vkName(code);
    toast(`「${label}」已设为 ${shown} — 记得回主界面点「保存」`, 'ok');
  } catch (e) {
    toast('改键失败：' + e.message, 'err');
    await refreshHotkeys();
  }
}

function refreshUpdate() {
  // ★ 插件 DLL 不是最新（游戏在运行时更新无法替换 DLL）→ 状态栏常驻警告 + 弹窗说明
  {
    const dw = document.getElementById('dllWarn');
    if (dw) dw.hidden = !(ST.upd && ST.upd.dllStale);
  }
  // ★ 游戏在运行时，插件 DLL 被占用：更新前先提醒（否则用户以为更新成功、其实没换）
  const running = !!(ST.live && ST.live.attached);
  const hint = $('#updDllHint');
  if (hint) {
    const stale = !!(ST.upd && ST.upd.dllStale);
    hint.hidden = !(running || stale);
    hint.textContent = stale
      ? ('⚠ 插件 DLL 不是最新版：' + (running ? '游戏正在运行，无法替换 DLL。' : '') + '请关闭游戏后点「立即更新」。')
      : (running ? '⚠ 游戏正在运行：更新时插件 DLL 无法替换，建议先关闭游戏再更新。' : '');
  }
  const u = ST.upd;
  $('#updCur').textContent = 'v' + (ST.version || '—');
  // 同版本号但仓库资产更新（热修复包）：明确告诉用户"为什么要更新"
  $('#updNew').textContent = (u.tag || '—') + (u.sameVer ? '（同版本号，仓库有更新的修复包）' : '');
  $('#updNotes').innerHTML = notesToHtml(u.notes || '');
  $('#updMsg').textContent = u.err ? ('出错：' + u.err) : (u.msg || '');
  const bar = $('#updBar');
  bar.hidden = !(u.state === 1 && u.progress > 0);
  $('#updBarFill').style.width = (u.progress || 0) + '%';
  $('#updInstall').hidden = !(u.state === 3);
  $('#updRestart').hidden = !u.needRestart;
}

/* ===========================================================================
   工具栏动作
   =========================================================================== */
const ACTIONS = {
  'cfg.save':   async () => {
    const r = await Backend.call('config.save', {});
    toast('已保存：' + (r.iniPath || ''), 'ok'); await refresh();
  },
  'cfg.saveAs': async () => {
    const r = await Backend.call('config.saveAs', {});
    if (!r.cancelled) { toast('已另存为：' + r.iniPath, 'ok'); await refresh(); }
  },
  'cfg.open':   async () => {
    const r = await Backend.call('config.open', {});
    if (!r.cancelled) { toast('已打开：' + r.iniPath, 'ok'); await refresh(); }
  },
  'cfg.mergeOld': async () => {
    const r = await Backend.call('config.mergeOldIni', {});
    await refresh();
    toast(r.added ? `已合并 ${r.added} 条` : '没有可合并的新条目', 'ok');
  },
  'game.reload': async () => { await Backend.call('game.reload', {});
    toast('已请求游戏重载配置（改完要先保存）', 'ok'); },
  'game.stop':   async () => { await Backend.call('game.stop', {});
      toast('已停止：游戏内音效 + GUI 试听', 'ok'); },
  'game.launch': async () => {
    const r = await Backend.call('game.launch', {});
    toast(r.running ? '怪物猎人已在运行，已切到游戏窗口' : '已启动怪物猎人', 'ok');
  },
  'sound.openDir': async () => { await Backend.call('sound.openSoundsDir', {}); },
  'entry.lint':  async () => {
    const r = await Backend.call('entries.lint', { weapon: ST.weaponFilter });
    // confirmBox 用 innerHTML：必须 <br> 换行，否则多条问题会挤成一行
    const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;');
    const lines = (r.issues || []).map(s => '• ' + esc(s)).join('<br>');
    await confirmBox({ title: '配置检查', message: lines || '没有发现问题', okText: '知道了' });
  },
  'entry.merge': async () => {
    const w = ST.weaponFilter;
    if (w < 0) { toast('先在左栏选一把武器，再合并该武器当前组合里的重复条目', 'err'); return; }
    const go = await confirmBox({
      title: '合并同条件条目', danger: false, okText: '合并',
      message: '会把「触发条件完全相同」的条目合并成一条：音效池取并集、重复的音效只保留一份，其余条目删除。\n'
             + '只处理当前武器的当前组合；合并后记得点保存。',
    });
    if (!go) return;
    const r = await Backend.call('entries.mergeSame', { weapon: w });
    await refresh();
    toast(r.merged ? `已合并 ${r.merged} 条同条件条目（剩余 ${r.left} 条）` : (r.status || '没有可合并的条目'),
          r.merged ? 'ok' : 'err');
  },
  'entry.add':  async () => { ST.selIndex = -1; renderTable(); await openEditor(null); },

  'combo.create': async () => {
    const inp = $('#comboNewName');
    const name = inp.value.trim();
    if (!name) { toast('先填组合名', 'err'); return; }
    if (ST.combos.includes(name)) { toast('这个组合名已经有了', 'err'); return; }
    if (ST.weaponFilter < 0) { toast('先在左栏选一把武器，再新建组合', 'err'); return; }
    await Backend.call('combo.create', { weapon: ST.weaponFilter, name });
    inp.value = '';
    await loadCombos(); renderComboPanel(); await refresh();
    setOnlyActive(true);   // 新组合是空的，默认只看它（不显示别的组合的"未激活"条目）
    toast('已新增空白组合：' + name + '（记得保存）', 'ok');
  },
  'combo.copyTo': async () => {
    const w = ST.weaponFilter;
    if (w < 0) { toast('先在左栏选一把武器，再复制组合', 'err'); return; }
    const from = ST.active[w] || '';
    ST.copySrc = { w, from };
    $('#copyFrom').textContent = weaponName(w) + ' · ' + (from || '默认');
    const ws = $('#copyWeapon');
    ws.innerHTML = '';
    (ST.weapons || []).forEach(x => ws.appendChild(h('option', { value: String(x.id), text: x.name })));
    ws.value = String(w);
    ws.onchange = () => fillCopyCombos();
    $('#copyReplace').checked = false;
    openModal('#mCopyCombo');
    await fillCopyCombos();
  },
  'combo.copyDo': async () => {
    const src = ST.copySrc || {};
    if (src.w == null) { toast('先选来源组合', 'err'); return; }
    const tw = parseInt($('#copyWeapon').value, 10);
    const tc = $('#copyCombo').value || '';
    try {
      const r = await Backend.call('combo.copyEntries', {
        fromWeapon: src.w, fromCombo: src.from, toWeapon: tw, toCombo: tc,
        replace: $('#copyReplace').checked,
      });
      closeModals();
      await refresh();
      toast(`已复制 ${r.copied} 条条目到「${weaponName(tw)} · ${tc || '默认'}」`, 'ok');
    } catch (e) { toast('复制失败：' + e.message, 'err'); }
  },
  'combo.rename': async () => {
    const w = ST.weaponFilter, from = ST.active[w] || '';
    if (!from) { toast('「默认」组合不能改名', 'err'); return; }
    const to = await confirmBox({ title: '组合改名', input: true, okText: '改名',
      message: `把组合「${from}」改成：`, placeholder: '新组合名' });
    if (!to) return;
    await Backend.call('combo.rename', { weapon: w, from, to });
    await loadCombos(); renderComboPanel(); await refresh();
    toast('已改名为「' + to + '」', 'ok');
  },
  'combo.del': async () => {
    const w = ST.weaponFilter, name = ST.active[w] || '';
    if (!name) { toast('「默认」组合不能删除', 'err'); return; }
    const n = ST.entries.filter(e => e.weaponType === w && e.combo === name).length;
    const ok = await confirmBox({ title: '删除组合', danger: true, okText: '删除组合',
      message: `确定删除组合「${name}」吗？\n\n` +
        (n ? `它下面的 <b>${n} 个条目</b>也会一起删掉。\n` : '这个组合下没有条目。\n') +
        `\n<b>删除后不可撤销</b>。` });
    if (!ok) return;
    const r = await Backend.call('combo.remove', { weapon: w, name });
    await loadCombos(); renderComboPanel(); await refresh();
    toast(`已删除组合「${name}」${r.removedEntries ? ' 及其 ' + r.removedEntries + ' 个条目' : ''}`, 'ok');
  },
  'combo.up':   async () => { await Backend.call('combo.move', { weapon: ST.weaponFilter, name: ST.active[ST.weaponFilter] || '', dir: -1 });
                              await loadCombos(); renderComboPanel(); },
  'combo.down': async () => { await Backend.call('combo.move', { weapon: ST.weaponFilter, name: ST.active[ST.weaponFilter] || '', dir: 1 });
                              await loadCombos(); renderComboPanel(); },
  'combo.export': async () => {
    // 导出的是"某一把武器的当前组合"，所以必须选中具体武器：
    // 选着"全部条目/通用"时 core 会直接报错，这里提前给一句人话，
    // 别让用户只看到一个两秒就消失的错误提示。
    const w = ST.weaponFilter;
    if (w < 0) { toast('先在左栏选一把武器，再导出该武器的组合', 'err'); return; }
    try {
      const r = await Backend.call('combo.export', { weapon: w });
      if (r.cancelled) return;
          if (r.missing > 0) {
            // ★ 把具体缺失路径列出来（用户/我们才能判断是路径解析问题还是文件真不在）
            const miss = (r.missingSample || []).map(s => '  · ' + s).join('\n');
            // 路径过长 / 无权限 / 被占用 这类失败：以管理员身份重新启动通常能解决（用户实测有效）
            // 触发建议的两种情况：① 原因像权限/路径问题；② 一个音效都没打包成功（多半就是权限/路径）
            const permish = /错误码|无权限|占用|过长|无法创建目标目录/.test(miss)
                            || (r.copied === 0 && r.missing > 0);
            const hint = permish
              ? '\n\n👉 上面若提示「路径过长 / 无权限 / 无法创建目标目录」：\n'
                + '请关闭本工具，右键「以管理员身份运行」后再导出一次（多数情况即可解决）。'
              : '';
            await confirmBox({ title: '导出完成，但有音效缺失', okText: '知道了',
              message: `条目 ${r.entries} 条，已打包音效 ${r.copied} 个，缺失 ${r.missing} 个。\n\n缺失的路径（前几个）：\n${miss || '(无样本)'}\n\n这些音效不会进包，别人导入后需要自己放。${hint}` });
            toast(`已导出「${ST.active[w] || '默认'}」：条目 ${r.entries} 条，音效 ${r.copied} 个，`
                  + `**${r.missing} 个音效文件缺失**。\n这个包不含缺失的音效，别人导入后需要自己放音效。\n${r.zip}`, 'err');
          } else {
            toast(`已导出「${ST.active[w] || '默认'}」：条目 ${r.entries} 条，音效 ${r.copied} 个\n${r.zip}`, 'ok');
          }
    } catch (e) { toast('导出失败：' + e.message, 'err'); }
  },
  'combo.import': async () => {
    const r = await Backend.call('combo.import', {});
    if (r.cancelled) return;
    await refresh();
    // ★ 用 core 真实返回的字段（soundCopied / soundMissing）：原来写的是 r.copied，
    //   该字段不存在 → 提示会显示"undefined 个音效"，用户看到还以为导入坏了。
    const sc = r.soundCopied || 0, sm = r.soundMissing || 0;
    if (sm > 0) {
      toast(`已导入 ${r.added} 条条目、${sc} 个音效；${sm} 个音效在包里没有（路径保持原样）。`
            + `\n这个组合包不含这些音效，需要自己把文件放到 sounds\\ 下。`, 'err');
    } else {
      toast(`已导入 ${r.added} 条条目、${sc} 个音效`, 'ok');
    }
  },

  'win.fsm':    async () => { openModal('#mFsm'); await runFsmSearch(); },
  'win.hot':    async () => { openModal('#mHot'); await refreshHotkeys(); },
  'win.ids':    async () => { openModal('#mIds'); await refreshIds(); },
  'overlay.toggle': async () => {
    const r = await Backend.call('overlay.toggle', {});
    const v = r && r.data ? r.data.visible : false;
    toast(v ? '捕获浮窗已打开（置顶显示捕获记录）' : '捕获浮窗已关闭', 'ok');
  },
  'win.update': async () => {
    openModal('#mUpd');
    const r = await Backend.call('update.check', { user: true });
    ST.upd = Object.assign(ST.upd, r); refreshUpdate();
  },
  'm.close': () => closeModals(),

  'ids.export': async () => { const r = await Backend.call('fsm.exportMeasured', {});
    toast(`已导出 ${r.count} 条到 ${r.path}`, 'ok'); await refreshIds(); },
  'ids.import': async () => { const r = await Backend.call('fsm.importCsv', {});
    if (!r.cancelled) { toast(`已合并 ${r.added} 条新动作ID`, 'ok'); await refreshIds(); } },
  'ids.submit': async () => { const r = await Backend.call('fsm.submit', {});
    toast('已写出 CSV 并复制到剪贴板，请在打开的页面粘贴提交', 'ok'); },
  'ids.fetch':  async () => { const r = await Backend.call('fsm.fetchLatest', {});
    toast(`已获取最新库（${r.count} 条）`, 'ok'); await refreshIds(); },

  'upd.check':   async () => { const r = await Backend.call('update.check', { user: true });
    ST.upd = Object.assign(ST.upd, r); refreshUpdate(); },
  'upd.install': async () => { await Backend.call('update.install', {});
    toast('开始下载安装…'); },
  'upd.restart': async () => { await Backend.call('update.restart', {}); },
  'upd.open':    async () => { await Backend.call('update.openLatest', {}); },

  // 清空：scope = all | derived | wem（capture.js 里的每个列表各有自己的清空按钮）
  'live.clearAll': async () => {
    try { await Backend.call('live.clear', { scope: 'all' }); } catch (e) { toast('清空失败：' + e.message, 'err'); }
    toast('已清空全部捕获', 'ok');
  },
  'live.clear': async () => {
    // 只清前端没用：实时事件一推，core 里的旧历史又会全量发回来。
    // 必须让 core 一起清。
    try { await Backend.call('live.clear', {}); } catch (e) { toast('清空失败：' + e.message, 'err'); }
    ST.live.history = [];
    // 顺手清掉"正在播放"的残留，否则清空后还挂着上一条
    ST.live.wemMedia = -1;
    ST.live.wemName = '';
    ST.live.wemBank = '';
    ST.live.wemSeqNum = 0;
    renderLive();
    toast('已清空 wem 捕获记录（等游戏里新的 wem 事件）', 'ok');
  },
  'live.clearWem': async () => {
    // nbnk 制作页的清空：只清 wem 捕获 + 清空日志文件，派生历史保留
    try { await Backend.call('live.clear', { kind: 'wem' }); } catch (e) { toast('清空失败：' + e.message, 'err'); }
    ST.live.history = (ST.live.history || []).filter(h => h.kind !== 1);
    ST.live.wemMedia = -1;
    ST.live.wemName = '';
    ST.live.wemBank = '';
    ST.live.wemSeqNum = 0;
    renderLive();
    toast('已清空 wem 捕获（派生历史保留）', 'ok');
  },
  'live.retry': () => { Backend.call('live.retry', {}).catch(() => {}); toast('已请求重新连接'); },

  'ed.close':  () => { $('#editor').classList.remove('open'); ED.open = false; },
  'ed.cancel': () => { $('#editor').classList.remove('open'); ED.open = false; },
  'ed.ok':     () => saveEditor(),
  'ed.applyJudge': () => { applyJudgePreset(); },
  'ed.addCond': () => {
    ED.draft.conds = ED.draft.conds || [];
    ED.draft.conds.push({ expr: 'dmg>0', atEnd: false, pool: [] });
    fillConds();
  },
};

/* ---- 给 wem 命名（用户库：WseWemNames_user.txt，下次自动读取）---- */
let nameDialogMedia = 0;
async function openNameDialog(media, fallbackName) {
  nameDialogMedia = media;
  $('#nameMedia').textContent = String(media);
  $('#nameCurrent').textContent = fallbackName || '（无）';
  $('#nameInput').value = '';
  openModal('#mName');
  try {
    const r = await Backend.call('wem.name.get', { media });
    $('#nameCurrent').textContent = r.name || '（无）';
    $('#nameInput').value = r.custom ? r.name : '';
    $('#nameLibPath').textContent = 'WseWemNames_user.txt（plugins\\WeaponSoundEnhance\\）';
  } catch (e) { /* 读不到就按空处理 */ }
  setTimeout(() => { const el = $('#nameInput'); if (el) el.focus(); }, 50);
}
ACTIONS['name.close'] = () => {
  // 只关命名框：从"音效替换"或"名字库"里弹出来的，不能连带关掉底下的窗口
  $('#mName').classList.remove('open');
  syncModalOpenClass();
};

ACTIONS['name.save'] = async () => {
  try {
    const r = await Backend.call('wem.name.set', { media: nameDialogMedia, name: $('#nameInput').value.trim() });
    if (r.name) toast('已命名为：' + r.name, 'ok');
    else toast('已删除自定义名（回退官方名字）', 'ok');
    // 只关命名框（从名字库进来时别把库一起关掉）
    $('#mName').classList.remove('open');
    syncModalOpenClass();
    renderBank();
    if ($('#mNameLib') && $('#mNameLib').classList.contains('open')) refreshNameLib();
  } catch (e) { toast('保存失败：' + e.message, 'err'); }
};
ACTIONS['name.clear'] = async () => {
  try {
    await Backend.call('wem.name.set', { media: nameDialogMedia, name: '' });
    toast('已删除自定义名（回退官方名字）', 'ok');
    $('#mName').classList.remove('open');
    syncModalOpenClass();
    renderBank();
    if ($('#mNameLib') && $('#mNameLib').classList.contains('open')) refreshNameLib();
  } catch (e) { toast('删除失败：' + e.message, 'err'); }
};

ACTIONS['wem.name.openDir'] = async () => {
  try { await Backend.call('wem.name.openDir', {}); } catch (e) { toast('打不开：' + e.message, 'err'); }
};

/* ---- 名字库管理（列出 / 搜索 / 删除）---- */
let nameLibItems = [];
async function openNameLib() {
  openModal('#mNameLib');
  const s = $('#nameLibSearch');
  if (s && !s.dataset.bound) { s.dataset.bound = '1'; s.oninput = () => renderNameLib(); }
  await refreshNameLib();
}
async function refreshNameLib() {
  try {
    const r = await Backend.call('wem.name.list', {});
    nameLibItems = r.items || [];
    if (r.file) $('#nameLibFile').textContent = r.file;
  } catch (e) { nameLibItems = []; }
  renderNameLib();
}
function renderNameLib() {
  const tb = $('#nameLibBody');
  if (!tb) return;
  tb.innerHTML = '';
  const q = ($('#nameLibSearch') ? $('#nameLibSearch').value : '').trim().toLowerCase();
  const items = nameLibItems.filter(it =>
    !q || String(it.media).indexOf(q) >= 0 || (it.name || '').toLowerCase().indexOf(q) >= 0);
  items.forEach(it => {
    tb.appendChild(h('tr', {},
      h('td', { class: 'fsm', style: 'color:var(--c-blue)' }, String(it.media)),
      h('td', {}, it.name),
      h('td', {}, h('div', { class: 'acts' },
        h('button', { onclick: () => openNameDialog(it.media, it.name) }, '改名'),
        h('button', { class: 'del', onclick: async () => {
          try {
            await Backend.call('wem.name.set', { media: it.media, name: '' });
            toast('已删除：' + it.name, 'ok');
            await refreshNameLib();
            renderBank();
          } catch (e) { toast('删除失败：' + e.message, 'err'); }
        } }, '删除')))));
  });
  const f = $('#nameLibFoot');
  if (f) f.textContent = `共 ${nameLibItems.length} 条自定义命名` +
    (q ? `（筛选出 ${items.length} 条）` : '');
}
ACTIONS['nameLib.search'] = () => renderNameLib();

ACTIONS['win.nameLib'] = () => { openNameLib(); };


/* ---- 音效替换（nbnk Mod）：必须在 const ACTIONS 之后注册，否则 TDZ 报错 ---- */
// ★ 音效替换是一个正式页面（不是弹窗）：切 mainEntries/footer ↔ mainBank
//   顺便把 WEM 捕获面板整个节点搬过去（nbnk 制作时要一边看 media 列表一边看游戏在播什么）
function showBankPage(on) {
  const a = $('#mainEntries'), b = $('#mainBank'), f = $('.status');
  if (a) a.hidden = !!on;
  if (b) b.hidden = !on;
  if (f) f.hidden = !!on;
  ST.bankPage = !!on;
  document.body.classList.toggle('bankpage-on', !!on);
  renderLive();   // 两个页面各有一份捕获面板（capture.js 渲染），切页只需重绘
}

// ---------------- nbnk 替换记录：导入时显示 / 恢复，导出后记住 ----------------
function bankRepsToArr() {
  return Object.keys(BANK.reps).map(k => ({ media: parseInt(k, 10), wem: BANK.reps[k] }))
                                        .filter(x => x.media > 0 && x.wem);
}
// ★ 说明行右侧的按钮/复选框：点它们时不要折叠 <details>。
//   注意不能在上层用 stopPropagation —— 捕获阶段 stop 会让按钮自己的 onclick 收不到事件（实测"点了没反应"）。
//   正确做法：在 summary 上 preventDefault，只取消默认的展开/收起。
document.addEventListener('click', e => {
  const t = e.target;
  if (!t || !t.closest) return;
  const inHdr = t.closest('#bankHistRow') || t.closest('.bankhdr-right label');
  if (inHdr) { const sm = t.closest('summary'); if (sm) e.preventDefault(); }
});

async function bankShowHistory() {
  const row = document.querySelector('#bankHistRow');
  if (!row) return;
  row.innerHTML = '';
  if (!BANK.path) return;
  try {
    const h = await Backend.call('bank.history.get', { path: BANK.path });
    if (!h.found || !(h.reps || []).length) return;
    const n = h.reps.length;
    const sp = document.createElement('span');
    sp.className = 'note ok';
    sp.style.marginRight = '8px';
    sp.textContent = `上次替换过 ${n} 个 wem（${h.time || ''}）`;
    const btn = document.createElement('button');
    btn.className = 'nav';
    btn.textContent = '恢复上次替换';
    btn.title = h.reps.map(x => x.media + ' → ' + x.wem).join('\n');
    btn.onclick = () => {
      BANK.reps = {};
      (h.reps || []).forEach(x => { if (x.media > 0 && x.wem) BANK.reps[x.media] = x.wem; });
      BANK.histSaved = true;
      renderBank();
      toast(`已恢复 ${n} 个替换（文件不在的会转换时报错，可重新选音频）`, 'ok');
          const _st = document.getElementById('statusText'); if (_st) _st.textContent = '已恢复上次替换：' + n + ' 个 wem';
    };
    row.appendChild(sp);
    row.appendChild(btn);
  } catch (e) {}
}
ACTIONS['bank.return'] = async () => {
  // 有替换但还没记进历史 → 问一下要不要保存（下次导入同一个 nbnk 能看见）
  const reps = bankRepsToArr();
  if (reps.length && !BANK.histSaved) {
    const go = await confirmBox({ title: '要保存这次的替换记录吗？', okText: '保存', danger: false,
      message: `这次替换了 ${reps.length} 个 wem。\n保存后，下次导入同一个 nbnk 就能看到这次替换了哪些，并可一键恢复。` });
    if (go) {
      try { await Backend.call('bank.history.save', { path: BANK.path, reps }); BANK.histSaved = true; toast('已保存替换记录', 'ok'); }
      catch (e) { toast('保存失败：' + e.message, 'err'); }
    }
  }
  showBankPage(false);
};

ACTIONS['win.bank'] = () => {
  // ★ 已在音效替换页面时，再点一次这个按钮 = 返回条目列表
  if (ST.bankPage) { showBankPage(false); return; }
  showBankPage(true);
  renderBank();
  loadBankOutDir();
  refreshConvertEnv();
};

// 转换环境自检（有没有 Wwise / ffmpeg / vgmstream）
async function refreshConvertEnv() {
  const el = $('#bankEnv');
  if (!el) return;
  try {
    const r = await Backend.call('wem.convertEnv', {});
    const wwise = r.hasWwise
      ? 'Wwise ✓（可把 wav/mp3/ogg 转成 wem）'
      : '未检测到 Wwise ✗ —— 只能替换「已经是 .wem」的文件；要转换其他格式需安装 Wwise（免费，Audiokinetic 官网）';
    const ff = r.hasFfmpeg ? 'ffmpeg ✓' : 'ffmpeg ✗（mp3/ogg 转换需要）';
    const vg = r.hasVgmstream ? '试听解码 ✓' : '试听解码 ✗（缺 wemkit\\vgmstream）';
    el.textContent = '转换环境：' + wwise + '　|　' + ff + '　|　' + vg;
    // ★ 缺 ffmpeg 时给一键下载（约 80MB，静态单文件版，不需要额外 DLL）
    if (!r.hasFfmpeg) {
      const st = await Backend.call('ffmpeg.status', {});
      const btn = document.createElement('button');
      btn.className = 'nav';
      btn.style.marginLeft = '8px';
      if (st.state === 1) {
        btn.textContent = '正在下载 ffmpeg… ' + (st.progress >= 0 ? st.progress + '%' : '');
        btn.disabled = true;
        setTimeout(refreshConvertEnv, 1000);   // 轮询进度
      } else {
        btn.textContent = '一键下载 ffmpeg（约 80MB）';
        btn.onclick = async () => {
          btn.disabled = true;
          btn.textContent = '开始下载…';
          try { await Backend.call('ffmpeg.download', {}); } catch (e) { toast('下载失败：' + e.message, 'err'); }
          setTimeout(refreshConvertEnv, 800);
        };
      }
      el.appendChild(btn);
      if (st.state === 2) toast('ffmpeg 已就绪 ✓', 'ok');
      if (st.state === 4) toast('ffmpeg 下载失败：' + (st.error || '未知原因') + '（也可自己下载放到 wemkit\\ffmpeg.exe）', 'err');
    }
    const brief = document.querySelector('#bankEnvBrief');
    if (brief) brief.textContent = r.hasWwise ? '（Wwise ✓）' : '（未检测到 Wwise）';
    el.className = r.hasWwise ? 'note ok' : 'note warn';
  } catch (e) { el.textContent = ''; }
}

// 输出目录（nbnk 导出位置）
async function loadBankOutDir() {
  try {
    const r = await Backend.call('bank.outDir', {});
    const el = $('#bankOutDir');
    if (el) el.value = r.dir || '';
    // 已经是游戏目录时，把「设为游戏目录」显示成「已是游戏目录」并禁用 ——
    // 这个按钮只是改目录，不导出；文案必须让人一眼看懂，免得以为点了就导出。
    const btn = $('#bankGameDirBtn');
    if (btn) {
      btn.disabled = !!r.isGameDir;
      btn.textContent = r.isGameDir ? '已是游戏目录' : '设为游戏目录';
      btn.title = r.isGameDir
        ? '当前输出目录就是游戏读取 nbnk 的位置；要生成文件请点「转换并导出」'
        : '只是把上面的「输出目录」改成游戏读取 nbnk 的位置（不会立刻导出，导出要点「转换并导出」）';
    }
  } catch {}
}
ACTIONS['bank.pickOutDir'] = async () => {
  try {
    const r = await Backend.call('bank.pickOutDir', {});
    const el = $('#bankOutDir');
    if (el) el.value = r.dir || '';
    if (r.changed) toast('输出目录已改为：' + r.dir, 'ok');
  } catch (e) { toast('选择目录失败：' + e.message, 'err'); }
};

ACTIONS['bank.pick'] = async () => {
  const r = await Backend.call('bank.pick', {});
  if (r.cancelled || !r.path) return;
  try {
    const info = await Backend.call('bank.inspect', { path: r.path });
    BANK.path = r.path;
    BANK.name = info.bank;
    BANK.media = info.media || [];
    BANK.histSaved = false;
    bankShowHistory();   // 同一个 nbnk：显示上次替换了哪些 wem（可一键恢复）
    BANK.reps = {};
    renderBank();
    if (!info.hasWwise)
      toast('没检测到 Wwise —— 生成 wem 需要它（可只替换已经是 wem 的文件）', 'err');
    else toast(`已导入 ${info.bank}.nbnk（${BANK.media.length} 条 media）`, 'ok');
  } catch (e) { toast('导入失败：' + e.message, 'err'); }
};

ACTIONS['bank.clear'] = () => { BANK.reps = {}; renderBank(); toast('已清空替换列表'); };

ACTIONS['bank.outDirToGame'] = async () => {
  try {
    const r = await Backend.call('bank.outDirToGame', {});
    const el = $('#bankOutDir');
    if (el) el.value = r.dir || '';
    toast('输出目录已设为游戏目录（还没导出：点「转换并导出」才会生成 nbnk）', 'ok');
  loadBankOutDir();
  } catch (e) { toast('设置失败：' + e.message, 'err'); }
};

ACTIONS['bank.openOut'] = async () => {
  try {
    const r = await Backend.call('bank.openOut', {});
    if (r && r.dir) toast('已打开：' + r.dir, 'ok');
  } catch (e) { toast('打不开：' + e.message, 'err'); }
};

ACTIONS['bank.export'] = async () => {
  if (!BANK.path) { toast('先导入 nbnk', 'err'); return; }
  const mediaIds = Object.keys(BANK.reps);
  if (!mediaIds.length) { toast('还没有选任何替换音效', 'err'); return; }
  const btn = ($('#bankExportBtn2') || $('#bankExportBtn'));
  btn.disabled = true;
  try {
    // 1) 转 wem（同一个进度里按需转换；已是 .wem 的直接用）
    const directWem = {};       // mediaId -> wem 路径（本来就是 wem）
    const needConv = [];        // 需要转换的音频
    mediaIds.forEach(id => {
      const f = BANK.reps[id];
      if (/\.wem$/i.test(f)) directWem[id] = f;
      else needConv.push({ id, file: f });
    });
    const wemOf = Object.assign({}, directWem);
    if (needConv.length) {
      toast(`正在转换 ${needConv.length} 个音频为 wem…（可能要几十秒）`, 'ok');
      const res = await Backend.call('wem.convert', { files: needConv.map(x => x.file) });
      const wems = res.wems || [];
      // 按文件名（不含扩展名）配对
      needConv.forEach(x => {
        const base = x.file.split(/[\\/]/).pop().replace(/\.[^.]+$/, '').toLowerCase();
        const hit = wems.find(w => w.split(/[\\/]/).pop().replace(/\.wem$/i, '').toLowerCase() === base);
        if (hit) wemOf[x.id] = hit;
      });
    }
    const reps = mediaIds.filter(id => wemOf[id]).map(id => ({ media: parseInt(id, 10), wem: wemOf[id] }));
    if (!reps.length) { toast('转换后没有可用的 wem', 'err'); return; }

    // 2) 导出 nbnk（写新文件，不动游戏原文件）
    const out = await Backend.call('bank.export', { path: BANK.path, replacements: reps });
    toast(`已导出 ${out.replaced} 条 → ${out.out}；进游戏切换一次武器种类再切回即可生效（不用重启）`, 'ok');
    // ★ 记住这次的替换：下次导入同一个 nbnk 会显示"上次替换了哪些 wem"，并可一键恢复
    try {
      await Backend.call('bank.history.save', { path: BANK.path, reps });
      BANK.histSaved = true;
      bankShowHistory();
    } catch (err) {}
    $('#bankInfo').textContent =
      `导出完成：${out.out}\n把该 nbnk 放到 游戏目录\\nativePC\\sound\\wwise\\Windows\\ 下即可生效。`;
  } catch (e) {
    toast('导出失败：' + e.message, 'err');
  } finally { btn.disabled = false; }
};

// 把导出的 nbnk 打包成可直接分享的 Mod 压缩包（结构从 nativePC 开始）
ACTIONS['bank.exportModZip'] = async () => {
  if (!BANK.path) { toast('先导入一个 nbnk', 'err'); return; }
  try {
    const r = await Backend.call('bank.exportModZip', { path: BANK.path });
    if (r.cancelled) return;
    toast('已导出 Mod 压缩包：解压到游戏根目录即可\n' + r.zip, 'ok');
  } catch (e) { toast('导出失败：' + e.message, 'err'); }
};

/* ===========================================================================
   刷新 / 启动
   =========================================================================== */
async function loadEntries() {
  const r = await Backend.call('entries.list', {});
  ST.entries = r.entries || [];
}

async function refresh() {
  const s = await Backend.call('state', {});
  ST.version = s.version;
  { const dw = document.getElementById('dllWarn'); if (dw) dw.hidden = !s.dllStale; }   // 插件 DLL 旧了就在状态栏警告（不依赖游戏/联网）
  // ★ 浮窗页面（index.html#overlay）：只显示捕获面板，其余全隐藏（见 CSS .overlay-mode）
  if (location.hash === '#overlay') document.documentElement.classList.add('overlay-mode');   // 与 head 里的同步标记一致
  ST.themes = s.themes || [];
  ST.weapons = s.weapons || [];
  ST.anyCount = s.anyCount || 0;
  ST.entryCount = s.entryCount || 0;
  ST.judges = s.judges || [];
  ST.global = s.global || {};
  ST.hotkeys = s.hotkeys || {};
  ST.active = s.active || {};
  ST.assetBase = s.assetBase || ST.assetBase;
  ST.iconBase = s.iconBase || ST.iconBase;
  ST.logoUrl = s.logoUrl || ST.logoUrl;

  if (s.ui) {
    if (s.ui.sort && s.ui.sort.key) ST.sort = s.ui.sort;
    if (typeof s.ui.weaponFilter === 'number') ST.weaponFilter = s.ui.weaponFilter;
    if (typeof s.ui.histExpanded === 'boolean') ST.histExpanded = s.ui.histExpanded;
    if (typeof s.ui.onlyActive === 'boolean') {
      ST.onlyActive = s.ui.onlyActive;
      const cb = $('#onlyActive');
      if (cb) cb.checked = s.ui.onlyActive;
    }
    if (typeof s.ui.wemView === 'boolean') {
      ST.wemView = s.ui.wemView;
      const lf = $('#viewListFsm'), lw = $('#viewListWem');
      if (lf && lw) { lf.className = 'vs' + (s.ui.wemView ? '' : ' on'); lw.className = 'vs' + (s.ui.wemView ? ' on' : ''); }
    }
  }
  if (!ST.ready) { applyTheme(s.theme || s.defaultTheme || 'clean-light'); renderThemePicker(); }

  $('#logoImg').src = ST.logoUrl;
  // 版本徽章顺带显示当前主题 id，方便一眼确认主题有没有生效
  $('#verText').textContent = 'v' + (s.version || '—') + ' · ' + (ST.theme || '?');
  $('#statusText').textContent = s.iniPath
    ? ((s.isGameIni ? '✓ 已加载（游戏读取的那份）：' : '⚠ 正在编辑的不是游戏读取的那份：') + s.iniPath)
    : '未加载配置';

  // 全局选项
  const g = ST.global;
  $('#gEnabled').checked = !!g.enabled;
  $('#gMore').checked = !!g.moreSounds;
  $('#gVol').value = g.volume == null ? 50 : g.volume;
  $('#gVolNum').textContent = String(g.volume == null ? 50 : g.volume);
  paintVolBar();   // ★ 数值变了必须重画填充条，否则数字和进度对不上
  $('#gDebug').checked = !!g.debug;
  $('#gHotkeys').checked = !!g.hotkeysEnabled;

  // 条目列表和组合列表互不依赖，并行拉取省一次往返
  await Promise.all([loadEntries(), loadCombos()]);
  await loadWeaponOn();   // ★ 武器触发开关（默认全关）
  renderWeapons();
  renderComboPanel();
  applyFilter();
  renderLive();
  ST.ready = true;
}

// 全局设置写入（主界面选项行与热键窗口都要用，放模块级共用）
function globalSet(k, v) {
  return Backend.call('global.set', { key: k, value: v })
    .catch(e => toast('保存设置失败：' + e.message, 'err'));
}

// 任何 range 滑块的"填充进度"都靠 background 渐变画出来（浏览器原生不画），
// 所以每次 value 变化都必须重画，否则填充位置停在 CSS 默认的 50% 上
//（表现为"一半蓝一半灰、与实际数值不符"）。
function paintRange(el) {
  const min = parseFloat(el.min) || 0;
  const max = parseFloat(el.max) || 100;
  const raw = parseFloat(el.value);
  const pct = max > min ? Math.max(0, Math.min(100, (raw - min) / (max - min) * 100)) : 0;
  el.style.background = `linear-gradient(90deg,var(--accent) ${pct}%, var(--line-2) ${pct}%)`;
}
function paintVolBar() {
  const el = $('#gVol');
  if (el) paintRange(el);
}

function wireGlobal() {
  const set = globalSet;
  $('#gEnabled').onchange = e => set('enabled', e.target.checked ? 1 : 0);
  $('#gMore').onchange = e => set('moreSounds', e.target.checked ? 1 : 0);
  $('#gDebug').onchange = e => set('debug', e.target.checked ? 1 : 0);
  $('#gHotkeys').onchange = e => set('hotkeysEnabled', e.target.checked ? 1 : 0);
  $('#gVol').oninput = e => {
    const v = parseInt(e.target.value, 10);
    $('#gVolNum').textContent = String(v);
    paintVolBar();
    set('volume', v);
  };
  paintVolBar();
}

function wireToolbar() {
  $$('[data-act]').forEach(el => {
    const fn = ACTIONS[el.dataset.act];
    if (!fn) return;
    el.addEventListener('click', () => { try { fn(); } catch (e) { toast(String(e.message || e), 'err'); } });
  });
  $$('#entryTable th.sortable').forEach(th => {
    th.addEventListener('click', () => {
      const k = th.dataset.sort;
      if (ST.sort.key === k) ST.sort.asc = !ST.sort.asc;
      else ST.sort = { key: k, asc: true };
      applyFilter();
      Backend.call('ui.set', { sort: ST.sort }).catch(() => {});
    });
  });
  $('#searchBox').addEventListener('input', e => { ST.search = e.target.value; applyFilter(); });
  $('#onlyActive').addEventListener('change', e => {
    ST.onlyActive = e.target.checked; applyFilter();
    Backend.call('ui.set', { onlyActive: ST.onlyActive }).catch(() => {});
  });
  $('#fsmQ').addEventListener('input', runFsmSearch);
  $('#fsmW').addEventListener('change', runFsmSearch);
  $('#mConfirm').addEventListener('click', e => { if (e.target.id === 'mConfirm') $('#cfCancel').click(); });
  $$('.modal').forEach(m => m.addEventListener('click', e => { if (e.target === m) closeModals(); }));
  document.addEventListener('keydown', e => {
    // ★ 改键模式必须最先吃掉按键：否则想按 Esc 取消反而会把窗口关掉
    if (hkCapture) {
      e.preventDefault();
      e.stopPropagation();
      if (e.keyCode === 27 || e.key === 'Escape') {
        hkCapture = null; renderHotkeys(); toast('已取消改键');
        return;
      }
      setHotkey(hkCapture, e.keyCode);
      return;
    }
    if (e.key === 'Escape') { closeModals(); if (!$('#mConfirm').classList.contains('open')) ACTIONS['ed.cancel'](); }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') { e.preventDefault(); ACTIONS['cfg.save'](); }
  });

  // ---- 热键窗口里的组合键 / 目标音量 / 聊天指令开关 ----
  $('#hkMod').addEventListener('change', e => setHotkey('modifierKey', parseInt(e.target.value, 10)));
  $('#hkVolVal').addEventListener('change', e => {
    const v = Math.max(0, Math.min(100, parseInt(e.target.value, 10) || 0));
    e.target.value = String(v);
    setHotkey('setVolValue', v);
  });
  $('#gChatCmd').addEventListener('change', e => {
    globalSet('chatCommands', e.target.checked ? 1 : 0);
  });
  $('#gChatEcho').addEventListener('change', e => {
    globalSet('chatEcho', e.target.checked ? 1 : 0);
  });
}

function buildFsmWeaponSelect() {
  const sel = $('#fsmW');
  sel.innerHTML = '';
  sel.appendChild(h('option', { value: '-1' }, '全部武器'));
  (ST.weapons || []).forEach(w => sel.appendChild(h('option', { value: String(w.id) }, `${w.id} ${w.name}`)));
}

// 数据就绪：通知宿主页面已接管（底层的启动加载画面由宿主管理，这里只发信号）
function uiReady() {
  if (Backend.mode === 'webview2') {
    Backend.call('ui.ready', {}).catch(() => {});
  }
}

Backend.onEvent(ev => {
    // ★ 另一端（浮窗/主界面）改了数据 → 同步刷新，避免两边不一致
    if (ev.event === 'refresh') { refresh(); return; }   // 另一端改了数据 → 同步（不 await，避免死循环）
    // 另一端换了主题 → 跟着换（浮窗与主界面主题保持一致）
    if (ev.event === 'theme' && ev.data && ev.data.theme) {
      document.documentElement.setAttribute('data-theme', ev.data.theme);
      ST.theme = ev.data.theme;
      return;
    }
  if (ev.event === 'live') {
    ST.live = Object.assign(ST.live, ev.data || {});
      // 插件 DLL 是否旧了（本地判定，随实时推送带过来）：状态栏常驻警告
      { const dw = document.getElementById('dllWarn'); if (dw) dw.hidden = !(ST.live && ST.live.dllStale); }
    renderLive();
  } else if (ev.event === 'update') {
    ST.upd = Object.assign(ST.upd, ev.data || {});
    refreshUpdate();
  } else if (ev.event === 'status') {
    if (ev.data && ev.data.text) $('#statusText').textContent = ev.data.text;
  }
});

(async function main() {
  // 预览用：?theme=pro-dark 可以强制某套主题（不写盘，只影响本次显示）
  const forcedTheme = new URLSearchParams(location.search).get('theme')
    || (location.hash.indexOf('#theme=') === 0 ? location.hash.slice(7) : null);

  // 有任何脚本错误都显式摆到界面上，别静默白屏
  window.addEventListener('error', ev => {
    const el = $('#statusText');
    if (el) el.textContent = '脚本错误：' + (ev.message || ev.error);
  });
  window.addEventListener('unhandledrejection', ev => {
    toast('操作失败：' + ((ev.reason && ev.reason.message) || ev.reason), 'err');
  });

  wireToolbar();
  // Esc：在音效替换页面按 Esc 返回条目列表（有弹窗打开时先不处理，交给弹窗）
  window.addEventListener('keydown', ev => {
    if (ev.key !== 'Escape') return;
    if (document.querySelector('.modal.open')) return;
    if (ST.bankPage) showBankPage(false);
  });
  wireGlobal();
  wireWemView();            // 派生 / WEM 捕获视图切换
  bindLogoEgg();            // 左上角图标彩蛋
  renderChatCmds();          // 指令表是静态的，建一次就行
  const tBoot = Date.now();
  try {
    await refresh();
    renderLive();   // 首屏必须渲染一次捕获面板：live 推送只在"有变化"时来，不能指望它
    uiReady();
    if (forcedTheme) applyTheme(forcedTheme);
    buildFsmWeaponSelect();
    // 预览用：#editor 直接打开第一条的编辑器（方便截图/试样式）
    if (location.hash.indexOf('editor') >= 0 && ST.visible.length) await openEditor(ST.visible[0].i);
    if (location.hash.indexOf('hot') >= 0) await ACTIONS['win.hot']();
    if (Backend.mode === 'browser')
      toast('浏览器预览模式：数据是假的，只用来调界面', 'ok');
  } catch (e) {
    uiReady();   // 出错也要撤加载屏，让错误显示出来
    $('#statusText').textContent = '初始化失败：' + e.message;
    toast('初始化失败：' + e.message, 'err');
  }
})();

/* ===========================================================================
   导出给独立模块（js/capture.js）用的最小 API。
   app.js 整体包在 IIFE 里，$ / $$ / h / ST / Backend 都不是全局 —— 必须显式导出，
   否则外部模块里会出现 "$$ is not defined" 这类错误（踩过一次）。
   ★ 防御式写法：逐个 typeof 检查，任何一个缺失都不至于让整个导出抛错。
   =========================================================================== */
window.SonarAPI = (function () {
  const api = {};
  function put(name, val) { if (typeof val !== 'undefined') api[name] = val; }
  try {
    put('$', $); put('$$', $$); put('h', h);
    put('ST', ST); put('Backend', Backend); put('toast', toast);
    put('ACTIONS', ACTIONS);
    put('wemDisplayName', wemDisplayName);
    put('captureToEntry', captureToEntry);
    put('addWemRecord', addWemRecord);
    put('openNameDialog', openNameDialog);
    put('sceneLabel', sceneLabel);
    put('isPeaceZone', isPeaceZone);
    put('renderTable', renderTable);
    put('renderWeapons', renderWeapons);
    put('weaponName', weaponName);
    Object.defineProperty(api, 'BANK', { get: function () { return BANK; }, enumerable: true });
  } catch (e) {
    api.__error = String(e);
  }
  api.__missing = ['$', '$$', 'h', 'ST', 'Backend', 'toast', 'ACTIONS', 'wemDisplayName',
    'captureToEntry', 'addWemRecord', 'openNameDialog', 'sceneLabel', 'isPeaceZone',
    'renderTable', 'renderWeapons'].filter(function (k) { return typeof api[k] === 'undefined'; });
  return api;
})();

// 兜底：若首屏渲染早于本模块导出完成，这里补渲染一次（不依赖 live 推送）
setTimeout(function () { try { if (typeof Capture !== 'undefined') Capture.render(); } catch (e) {} }, 60);

})();
