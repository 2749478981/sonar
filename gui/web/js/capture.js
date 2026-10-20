/* ===========================================================================
   Sonar 捕获模块（独立模块，2026 重做）

   为什么是独立文件：捕获面板以前散在 app.js 里，还靠"搬 DOM"在两个页面之间
   移动，出了问题（清空/过滤/视图）极难定位。现在：

   1) 两条**独立**列表：派生捕获 / WEM 捕获 —— core 各给一份数据 + 修订号，
      互不挤占、各自上限、各自清空。
   2) 渲染到**多个宿主**（主页面 / nbnk 制作页各一份静态容器），**不再搬 DOM**。
   3) 过滤条件**显式可见**：显示当前武器/bank 过滤 + "已隐藏 N 条"，可一键清除。
   4) 每个列表**自己的清空按钮**（派生归派生、wem 归 wem），语义不再混淆。
   5) 每一行 wem 都能**直接播放**：core 的 wem.play 会自动反查 media 属于哪个
      nbnk 文件（真值表 → 候选目录 → DIDX 索引兜底），不要求它属于当前导入的 bank。

   app.js 整体在 IIFE 里，所以这里一律通过 window.SonarAPI 取它的工具函数。
   =========================================================================== */
const Capture = (() => {
  const API = () => window.SonarAPI || null;

  const S = {
    view: 'derived',        // 'derived' | 'wem' | 'efx'
    expanded: false,
    lastDerRev: -1,
    lastWemRev: -1,
    lastEfxRev: -1,
    playingKey: '',   // 只高亮"被点的那一行"（media+时间），避免同 media 多行一起亮
  };

  function live() { const a = API(); return (a && a.ST && a.ST.live) || {}; }
  function el(tag, attrs, ...kids) { const a = API(); return a ? a.h(tag, attrs, ...kids) : null; }
  function $(s) { const a = API(); return a ? a.$(s) : null; }
  function $$(s) { const a = API(); return a ? a.$$(s) : []; }
  function toast(msg, kind) { const a = API(); if (a) a.toast(msg, kind); }
  function call(m, p) { const a = API(); return a ? a.Backend.call(m, p) : Promise.reject(new Error('后端不可用')); }
  function bank() { const a = API(); return (a && a.BANK) || {}; }

  // ---- 过滤（显式、可清除）----
  function weaponFilterOn() {
    if (document.documentElement.classList.contains('overlay-mode')) return false;   // 浮窗里不过滤：只看捕获记录，不受主界面武器筛选影响
    const a = API();
    return !!(a && a.ST && typeof a.ST.weaponFilter === 'number' && a.ST.weaponFilter >= 0);
  }
  function bankFilterOn() {
    if (document.documentElement.classList.contains('overlay-mode')) return false;
    const cb = $('#bankOnlyThis');
    return !!(bank().path && cb && cb.checked);
  }
  function filterList(rows) {
    const a = API();
    let out = rows || [];
    if (weaponFilterOn()) out = out.filter(r => r.weapon === a.ST.weaponFilter);
    if (bankFilterOn()) out = out.filter(r => (r.bank || '') === bank().name);
    return out;
  }
  function clearWeaponFilter() {
    const a = API();
    if (!a) return;
    a.ST.weaponFilter = -1;
    a.renderWeapons(); a.renderTable();
    call('ui.set', { weaponFilter: -1 }).catch(() => {});
    render();
  }
  function clearBankFilter() {
    const cb = $('#bankOnlyThis');
    if (cb) cb.checked = false;
    call('ui.set', { bankOnly: false }).catch(() => {});
    render();
  }

  // ---- 播放（core 自动反查 bank 文件）----
  async function playWem(r, ev) {
    if (ev && ev.stopPropagation) ev.stopPropagation();
    if (!r || !r.wemMedia) return;
    try {
      const res = await call('wem.play', { media: r.wemMedia, bank: r.bank || '' });
      S.playingKey = r.wemMedia + '@' + (r.time || '');
      // 播完明确告诉用户"播的是哪条、来自哪个 bank" —— 便于判断有没有播错
      const who = r.dispName || r.name || ('media ' + r.wemMedia);
      toast(`播放：${who}　·　${res.bank || 'nbnk'}（media ${r.wemMedia}）`, 'ok');
      render();
    } catch (e) {
      toast('播放失败：' + (e.message || e), 'err');
    }
  }

  function clearScope(scope) {
    call('live.clear', { scope })
      .then(() => toast(scope === 'wem' ? '已清空 WEM 捕获（派生保留）' : '已清空派生捕获（wem 保留）', 'ok'))
      .catch(e => toast('清空失败：' + (e.message || e), 'err'));
  }

  // ---- 小组件 ----
  function chip(text, onClear, title) {
    return el('span', { class: 'capchip', title: title || '' },
      el('span', { text }),
      el('b', { class: 'x', text: '✕', onclick: e => { e.stopPropagation(); onClear(); } }));
  }
  function btn(text, onclick, title, cls) {
    return el('button', { class: 'capbtn' + (cls ? ' ' + cls : ''), title: title || '', text,
      onclick: e => { e.stopPropagation(); onclick(e); } });
  }
  function row(attrs, ...kids) {
    // h() 只接 (tag, attrs, ...children)，事件放在 attrs 里
    return el('div', attrs, ...kids.filter(Boolean));
  }

  function derRow(r) {
    const a = API();
    return row({ class: 'hrec' + (r.added ? ' added' : ''), title: '点击把这个动作加入条目',
                 onclick: () => a.captureToEntry(r) },
      el('b', { text: r.time || '' }),
      el('span', { class: 'n', text: r.name || ('fsm ' + r.fsm) }),
      el('span', { class: 'ids', text: `w${r.weapon} ${r.fsm}/${r.lmt}` }),
      el('span', { class: 'badge' + ((r.matches || 0) > 1 ? ' warn' : ''), title: '该触发条件下共有几条条目', text: (r.matches || 0) > 1 ? ('×' + r.matches + ' 条条目') : '1 条条目' }),
      el('span', { class: 'capacts' },
        r.added ? btn('编辑', () => a.captureToEntry(r), '该动作已配了条目 → 编辑')
                : btn('＋ 添加', () => a.captureToEntry(r), '把这个动作加入条目', 'primary')));
  }

  function wemRow(r, page) {
    const a = API();
    const nm = r.dispName || a.wemDisplayName(r);
    const playing = S.playingKey === (r.wemMedia + '@' + (r.time || ''));
    const meta = `media ${r.wemMedia}` + (r.bank ? ' · ' + r.bank : '') +
      (r.seqNum ? ` · 第${r.seqNum}个` : '');
    // ★ 两行显示：第一行时间+名称，第二行 media 信息 + 按钮。
    //   以前一行塞 3 个按钮 + 长文本，必须横向拖动才能点到按钮。
    return el('div', { class: 'hrec wem two' + (r.added ? ' added' : '') + (playing ? ' playing' : ''),
                       title: '播放：' + nm + '（core 会自动找它所属的 nbnk）' },
      el('div', { class: 'l1', onclick: () => { if (page === 'bank') playWem(r); else a.addWemRecord(r); } },
        el('b', { text: r.time || '' }),
        el('span', { class: 'n', text: nm })),
      el('div', { class: 'l2' },
        el('span', { class: 'ids', text: meta }),
        el('span', { class: 'capacts' },
          btn(playing ? '▶ 播放中' : '▶ 播放', () => playWem(r), '在 GUI 内播放这条 wem', 'primary'),
          btn('命名', () => a.openNameDialog(r.wemMedia, nm), '给这条 wem 起名字（存进名字库）'),
          page === 'bank' ? null
            : (r.added ? btn('编辑', () => a.addWemRecord(r)) : btn('＋ 添加', () => a.addWemRecord(r))))));
  }

  // 特效行：表 · 组 · 记录（标识 = epv 表 + group + 组内记录下标）
  function efxRow(r) {
    const label = r.table + ' · 组' + r.group + ' · #' + r.rec + (r.via >= 0 ? '  [T' + r.via + ']' : '');
    const cands = (r.cand || '').split(',').filter(Boolean);
    const candTxt = cands.length > 1 ? ('　候选表:' + cands.join('/') ) : '';
    const meta = (r.name || '') + (r.recId >= 0 ? `　id=${r.recId}` : '') + candTxt +
                 (r.weapon >= 0 ? `　w${r.weapon}` : '');
    return el('div', { class: 'hrec efx two', title: label + '\n' + (r.name || '') },
      el('div', { class: 'l1' },
        el('b', { text: r.time || '' }),
        el('span', { class: 'n', text: label })),
      el('div', { class: 'l2' },
        el('span', { class: 'ids', text: meta })));
  }

  // ---- 一个宿主面板 ----
  function renderHost(host) {    const a = API();
    if (!a) { host.textContent = '界面模块未就绪（SonarAPI 缺失）'; return; }
    const page = host.dataset.page || 'main';
    if (S.view === 'efx' && !L.efxEnabled) S.view = 'derived';   // 模块关闭时不留空视图
    const L = live();
    host.innerHTML = '';

    // 连接状态
    const conn = el('div', { class: 'conn' },
      el('span', { class: 'dot' + (L.attached ? ' ok' : '') }),
      el('span', { text: L.attached
        ? ('已连接' + (L.pid ? ` (PID ${L.pid})` : '') + (L.inScene ? ' · 已进场景' : ' · 未进场景'))
        : (L.error || '未连接') }));
    host.appendChild(conn);

    // 场景
    const scene = a.sceneLabel(L.mapId);   // 已含"场景：xxx（地图 N）"整句，这里不要再加前缀/后缀
    if (scene) {
      const peace = (L.mapId > 0 && a.isPeaceZone(L.mapId)) ? ' · 和平区' : '';
      host.appendChild(el('div', { class: 'conn' },
        el('span', { text: scene }),
        el('span', { class: 'dim', text: (L.questState === 2 ? '任务中' : (L.questState >= 0 ? '非任务' : '')) + peace })));
    }

    // 视图切换（浮窗按钮在面板头部 .chead 里，见 index.html）
    host.appendChild(el('div', { class: 'viewswitch' },
      el('button', { class: 'vs' + (S.view === 'derived' ? ' on' : ''), text: '派生捕获',
        onclick: () => { S.view = 'derived'; render(); } }),
      el('button', { class: 'vs' + (S.view === 'wem' ? ' on' : ''), text: 'WEM 音效',
        onclick: () => { S.view = 'wem'; render(); } }),
      (L.efxEnabled ? el('button', { class: 'vs' + (S.view === 'efx' ? ' on' : ''), text: '武器特效',
        onclick: () => { S.view = 'efx'; render(); } }) : null)));

    // 过滤提示（显式 + 一键清除 + 隐藏条数）
    const allDer = L.der || [], allWem = L.wem || [];
    const der = filterList(allDer), wem = filterList(allWem);
    const hidden = (allDer.length - der.length) + (allWem.length - wem.length);
    if (weaponFilterOn() || bankFilterOn() || hidden > 0) {
      const bar = el('div', { class: 'capfilter' }, el('span', { class: 'dim', text: '过滤：' }));
      if (weaponFilterOn()) bar.appendChild(chip('武器 w' + a.ST.weaponFilter, clearWeaponFilter, '点击清除武器过滤'));
      if (bankFilterOn()) bar.appendChild(chip('bank ' + (bank().name || ''), clearBankFilter, '点击清除 bank 过滤'));
      if (hidden > 0) bar.appendChild(el('span', { class: 'warnchip', text: `已隐藏 ${hidden} 条` }));
      host.appendChild(bar);
    }

    // ---- 派生列表 ----
    if (S.view === 'derived') {
      // 派生实时行同样遵守武器过滤（避免"列表过滤了、顶上还显示"的错觉）
      const liveDerVisible = (L.inScene && L.fsm >= 0) &&
        (!weaponFilterOn() || L.weapon === (API() && API().ST ? API().ST.weaponFilter : -1));
      if (liveDerVisible) {
        host.appendChild(row({ class: 'live-row', title: '当前动作 → 点击加入条目',
                               onclick: () => a.captureToEntry(L) },
          el('b', { text: (L.added ? '✔ ' : '') + (L.name || ('fsm ' + L.fsm)) }),
          el('span', { class: 'dim', text: `w${L.weapon} ${L.fsm}/${L.lmt}` }),
          el('span', { class: 'capacts' },
            L.added ? btn('编辑', () => a.captureToEntry(L))
                    : btn('＋ 添加', () => a.captureToEntry(L), '', 'primary'))));
      }
      host.appendChild(el('div', { class: 'caplist-head' },
        el('span', { text: `派生捕获（${der.length}${der.length !== allDer.length ? '/' + allDer.length : ''} 条）` }),
        el('span', { class: 'grow' }),
        der.length ? btn('清空', () => clearScope('derived'), '只清空派生捕获，wem 保留') : null));

      const box = el('div', { class: 'hist' });
      if (!der.length) {
        box.appendChild(el('div', { class: 'note', text: allDer.length
          ? `派生记录有 ${allDer.length} 条，但被过滤条件隐藏了（点上面的 ✕ 清除过滤）`
          : (weaponFilterOn() ? '当前武器还没有派生记录：进游戏做动作后回来看'
                              : '还没有派生记录：进游戏做动作后回来看') }));
      } else {
        const show = S.expanded ? der : der.slice(0, 20);
        show.forEach(r => box.appendChild(derRow(r)));
        if (der.length > 20) {
          box.appendChild(row({ class: 'hrec more',
            text: S.expanded ? '收起历史 ▲' : `展开更早的 ${der.length - 20} 条 ▼`,
            onclick: () => { S.expanded = !S.expanded; render(); } }));
        }
      }
      host.appendChild(box);
    }

    // ---- WEM 列表 ----
    if (S.view === 'wem') {
      // 实验性说明：WEM 捕获是"抓游戏内正在播的 wem 再同步播放"，不是官方接口，
      // 受场景切换/循环音/同事件多次请求影响，不保证与实际发声完全一致。
      host.appendChild(el('div', { class: 'note exp', text:
        '⚠ 实验性：这里通过捕获游戏内的 wem 调用情况来同步播放音效。' +
        '可能受场景切换、循环音、以及同一事件被多次请求的影响，不保证与实际发声完全一致。' }));
      // ★ 实时行也要遵守过滤：否则列表被过滤了、顶上却还显示别的武器/bank 的 wem，
      //   看起来像"过滤没生效"
      const liveWemRow = { wemMedia: L.wemMedia, bank: L.wemBank, weapon: L.wemWeapon };
      const liveWemVisible = L.wemMedia > 0 && filterList([liveWemRow]).length > 0;
      if (L.wemMedia > 0 && !liveWemVisible) {
        host.appendChild(el('div', { class: 'note dim',
          text: '（当前正在播放的 wem 不在过滤范围内，已隐藏）' }));
      }
      if (liveWemVisible) {
        const nm = L.wemDispName || a.wemDisplayName({ wemMedia: L.wemMedia, name: L.wemName, note: L.wemNote,
                                                       custom: L.wemNameCustom, bank: L.wemBank, seqNum: L.wemSeqNum });
        host.appendChild(row({ class: 'live-row wem', title: '游戏正在播放的 wem' },
          el('b', { text: nm }),
          el('span', { class: 'dim', text: 'media ' + L.wemMedia + (L.wemBank ? ' · ' + L.wemBank : '') }),
          el('span', { class: 'capacts' },
            btn('▶ 播放', () => playWem({ wemMedia: L.wemMedia, bank: L.wemBank }), '在 GUI 内播放', 'primary'),
            btn('命名', () => a.openNameDialog(L.wemMedia, L.wemName || '')),
            page === 'bank' ? null : btn('＋ 添加', () => a.addWemRecord({
              kind: 1, wemMedia: L.wemMedia, name: L.wemName, bank: L.wemBank,
              weapon: L.wemWeapon || -1, added: L.wemAdded, seqNum: L.wemSeqNum })))));
      }
      host.appendChild(el('div', { class: 'caplist-head' },
        el('span', { text: `WEM 捕获（${wem.length}${wem.length !== allWem.length ? '/' + allWem.length : ''} 条）` }),
        el('span', { class: 'grow' }),
        wem.length ? btn('清空', () => clearScope('wem'), '只清空 WEM 捕获（含日志文件），派生保留') : null));

      const box = el('div', { class: 'hist' });
      if (!wem.length) {
        box.appendChild(el('div', { class: 'note', text: allWem.length
          ? `wem 记录有 ${allWem.length} 条，但被过滤条件隐藏了（点上面的 ✕ 清除过滤）`
          : '还没有 wem 记录：进游戏做动作后回来看（点行可直接播放）' }));
      } else {
        const show = S.expanded ? wem : wem.slice(0, 20);
        show.forEach(r => box.appendChild(wemRow(r, page)));
        if (wem.length > 20) {
          box.appendChild(row({ class: 'hrec more',
            text: S.expanded ? '收起历史 ▲' : `展开更早的 ${wem.length - 20} 条 ▼`,
            onclick: () => { S.expanded = !S.expanded; render(); } }));
        }
      }
      host.appendChild(box);
    }

    // ---- 武器特效列表（实验性）----
    if (S.view === 'efx') {
      host.appendChild(el('div', { class: 'note exp', text:
        '⚠ 实验性：通过挂钩引擎的特效生成函数，记录武器特效的调用（表 · 组 · 记录）。' +
        '只读、不改游戏行为；目前仅用于捕获，还不能据此播放音效。' }));
      // 自检状态：挂钩成功/失败原因（失败时明确说出来，不静默）
      const st = L.efxStatus || '';
      if (st) {
        const bad = st.indexOf('失败') >= 0 || st.indexOf('未找到') >= 0;
        host.appendChild(el('div', { class: 'note' + (bad ? ' warnchip' : ' dim'), text: '特效捕获：' + st }));
      if (L.efxIndex) host.appendChild(el('div', { class: 'note dim', text: '索引：' + L.efxIndex }));
      }
      // DLL 定期统计：区分「hook 没被调用」和「调用了但值不在索引里」
      if (L.efxStats) {
        host.appendChild(el('div', { class: 'note dim', text: '钩到调用：' + L.efxStats }));
      }
      if (!(L.efx || []).length && L.efxStatus && L.efxStatus.indexOf('挂钩成功') >= 0 &&
          L.efxStats && L.efxStats.indexOf('calls=0') >= 0) {
        host.appendChild(el('div', { class: 'note warnchip', text:
          '⚠ 挂钩成功、但一次调用都没钩到：这版游戏的武器特效可能不走这个函数（需要换挂钩点）。' }));
      }
      if (!(L.efx || []).length && L.efxRawSample) {
        host.appendChild(el('div', { class: 'note dim', text:
          '（已钩到调用，但都不在 hm_wp* 表里）取样：' + L.efxRawSample.slice(0, 220) }));
      }
      const allEfx = L.efx || [], efx = filterList(allEfx);
      // 实时行
      if (L.efxTable) {
        host.appendChild(row({ class: 'live-row efx', title: '游戏刚生成的特效' },
          el('b', { text: `${L.efxTable} · 组${L.efxGroup} · #${L.efxRec}` }),
          el('span', { class: 'dim', text: L.efxPath || '' }),
          el('span', { class: 'capacts' },
            btn('复制标识', () => {
              const txt = `${L.efxTable} ${L.efxGroup} ${L.efxRec}`;
              try { navigator.clipboard.writeText(txt); toast('已复制：' + txt, 'ok'); }
              catch (e) { toast('复制失败：' + txt, 'err'); }
            }, '复制「表 组 记录」标识'))));
      }
      host.appendChild(el('div', { class: 'caplist-head' },
        el('span', { text: `武器特效（${efx.length}${efx.length !== allEfx.length ? '/' + allEfx.length : ''} 条）` }),
        el('span', { class: 'grow' }),
        efx.length ? btn('清空', () => clearScope('efx'), '只清空特效捕获（含日志文件）') : null));

      const ebox = el('div', { class: 'hist' });
      if (!efx.length) {
        ebox.appendChild(el('div', { class: 'note', text: allEfx.length
          ? `特效记录有 ${allEfx.length} 条，但被过滤条件隐藏了（点上面的 ✕ 清除过滤）`
          : '还没有特效记录：进游戏、拿武器打几下再回来看' }));
      } else {
        const show = S.expanded ? efx : efx.slice(0, 20);
        show.forEach(r => ebox.appendChild(efxRow(r)));
        if (efx.length > 20) {
          ebox.appendChild(row({ class: 'hrec more',
            text: S.expanded ? '收起历史 ▲' : `展开更早的 ${efx.length - 20} 条 ▼`,
            onclick: () => { S.expanded = !S.expanded; render(); } }));
        }
      }
      host.appendChild(ebox);
    }

    // 统计（一行说清：事件到了多少、识别多少、被丢多少）
    const ok = L.wemOk || 0, zero = L.wemZero || 0, dup = L.wemDup || 0, loop = L.wemLoopDrop || 0;
    if (S.view === 'wem' && ok + zero + dup + loop > 0) {
      host.appendChild(el('div', { class: 'capstat',
        text: `识别 ${ok} · 未识别 ${zero} · 重复丢弃 ${dup} · 循环音静默 ${loop}` }));
    }
    if (S.view === 'efx' && ((L.efxOk || 0) + (L.efxDup || 0) > 0)) {
      host.appendChild(el('div', { class: 'capstat',
        text: `武器特效 ${L.efxOk || 0} · 重复丢弃 ${L.efxDup || 0}` }));
    }
  }

  function render() {
    const hosts = $$('.capHost');
    hosts.forEach(h => { try { renderHost(h); } catch (e) { h.textContent = '捕获渲染出错：' + e.message; } });
    const L = live();
    S.lastDerRev = L.derRev;
    S.lastWemRev = L.wemRev;
    S.lastEfxRev = L.efxRev;
  }

  // 收到 live 推送：修订号没变就不重绘（省 CPU、避免闪）
  function onPush(d) {
    if (!d) return;
    if (d.derRev === S.lastDerRev && d.wemRev === S.lastWemRev && d.efxRev === S.lastEfxRev) return;
    render();
  }

  return {
    render, onPush, playWem, clearScope,
    get view() { return S.view; },
    set view(v) { S.view = v; render(); },
  };
})();
