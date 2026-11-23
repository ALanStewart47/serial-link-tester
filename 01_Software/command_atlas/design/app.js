/* Command Atlas redesign · interactions */
(function () {
  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));

  /* ---------- stage switch ---------- */
  $$('#stageSwitch button').forEach((btn) => {
    btn.addEventListener('click', () => {
      const i = Number(btn.dataset.stage);
      $$('#stageSwitch button').forEach((b) => b.classList.toggle('active', b === btn));
      $$('.stage').forEach((s, idx) => s.classList.toggle('active', idx === i));
    });
  });

  /* ---------- frame model (sample: CA 01 ch bright BE, BCC) ---------- */
  const state = {
    channel: 1,
    brightness: 255,
    cmd: 'bright',
    hot: null, // 'channel' | 'bright' | null
  };

  const inCh = $('#inChannel');
  const inBr = $('#inBright');
  const pCh = $('#pChannel');
  const pBr = $('#pBright');
  const hexLine = $('#hexLine');
  const byteGroups = $('#byteGroups');
  const readable = $('#readable');
  const btnHex = $('#btnCopyHex');
  const btnAscii = $('#btnCopyAscii');
  const toast = $('#toast');
  const toastCode = $('#toastCode');

  function clampInt(v) {
    if (v === '' || v === null || v === undefined) return null;
    const n = Number(v);
    if (!Number.isFinite(n) || !Number.isInteger(n)) return null;
    return n;
  }

  function bcc(bytes) {
    let x = 0;
    for (let i = 0; i < bytes.length; i++) x ^= bytes[i];
    return x;
  }

  function hex2(n) {
    return n.toString(16).toUpperCase().padStart(2, '0');
  }

  function validate() {
    let ok = true;
    const ch = clampInt(inCh.value);
    const br = clampInt(inBr.value);

    const chBad = ch === null || ch < 1 || ch > 40;
    const brBad = br === null || br < 0 || br > 999;

    pCh.classList.toggle('has-error', chBad);
    inCh.classList.toggle('invalid', chBad);
    pBr.classList.toggle('has-error', brBad);
    inBr.classList.toggle('invalid', brBad);

    if (chBad || brBad) {
      ok = false;
    } else {
      state.channel = ch;
      state.brightness = br;
    }
    return ok;
  }

  function buildFrame() {
    // only brightness-set has params in this demo
    const ch = state.channel;
    const br = state.brightness;
    const head = [0xca, 0x01];
    const chB = [ch & 0xff];
    const brB = [(br >> 8) & 0xff, br & 0xff];
    const body = head.concat(chB, brB);
    const sum = bcc(body);
    return {
      groups: [
        { key: 'head', kind: 'fixed', label: '帧头', bytes: head },
        { key: 'cmd', kind: 'fixed', label: '命令', bytes: [0x01] },
        { key: 'channel', kind: 'param', label: '通道', value: String(ch), bytes: chB },
        { key: 'bright', kind: 'param', label: '亮度', value: String(br), bytes: brB },
        { key: 'bcc', kind: 'comp', label: 'BCC', bytes: [sum] },
      ],
      flat: body.concat([sum]),
    };
  }

  function renderAnatomy() {
    const frame = buildFrame();
    hexLine.textContent = frame.flat.map(hex2).join(' ');

    byteGroups.innerHTML = '';
    let offset = 0;
    frame.groups.forEach((g) => {
      const wrap = document.createElement('div');
      wrap.className = 'bgroup is-' + g.kind;
      wrap.dataset.key = g.key;

      const lab = document.createElement('div');
      lab.className = 'glabel';
      lab.innerHTML =
        g.kind === 'param'
          ? g.label + ' <em>' + g.value + '</em>'
          : g.label;
      wrap.appendChild(lab);

      const br = document.createElement('div');
      br.className = 'bracket';
      wrap.appendChild(br);

      const row = document.createElement('div');
      row.className = 'byte-row';
      g.bytes.forEach((b) => {
        const cell = document.createElement('div');
        cell.className = 'byte ' + (g.kind === 'param' ? 'param' : g.kind === 'comp' ? 'comp' : 'fixed');
        if (state.hot && g.key === state.hot) cell.classList.add('hot');
        cell.textContent = hex2(b);
        row.appendChild(cell);
      });
      wrap.appendChild(row);

      // offset strip under this group
      const offs = document.createElement('div');
      offs.className = 'offset-row';
      offs.style.justifyContent = 'center';
      g.bytes.forEach((_, i) => {
        const o = document.createElement('div');
        o.className = 'offset';
        o.textContent = String(offset + i);
        o.style.width = '42px';
        offs.appendChild(o);
      });
      wrap.appendChild(offs);

      byteGroups.appendChild(wrap);
      offset += g.bytes.length;
    });

    const hex = frame.flat.map(hex2);
    readable.innerHTML =
      '帧头 <b>' +
      hex[0] +
      '</b> · 命令 <b>' +
      hex[1] +
      '</b>（设置亮度）· 通道 <b>' +
      state.channel +
      '</b> · 亮度 <b>' +
      state.brightness +
      '</b>（字节 <b>' +
      hex[3] +
      ' ' +
      hex[4] +
      '</b>，大端）· 校验 <b>BCC = XOR(0..4) = ' +
      hex[5] +
      '</b>';

    return frame.flat.map(hex2).join(' ');
  }

  function refresh() {
    const ok = validate();
    const hexText = renderAnatomy();
    btnHex.disabled = !ok;
    btnAscii.disabled = !ok;
    if (ok) {
      toastCode.textContent = hexText;
    }
    return hexText;
  }

  function setHot(key) {
    state.hot = key;
    refresh();
  }

  inCh.addEventListener('input', refresh);
  inBr.addEventListener('input', refresh);
  inCh.addEventListener('focus', () => setHot('channel'));
  inBr.addEventListener('focus', () => setHot('bright'));
  inCh.addEventListener('blur', () => setHot(null));
  inBr.addEventListener('blur', () => setHot(null));

  // hover param form → highlight bytes
  pCh.addEventListener('mouseenter', () => setHot('channel'));
  pCh.addEventListener('mouseleave', () => setHot(null));
  pBr.addEventListener('mouseenter', () => setHot('bright'));
  pBr.addEventListener('mouseleave', () => setHot(null));

  // hover byte group → highlight form
  byteGroups.addEventListener('mouseover', (e) => {
    const g = e.target.closest('.bgroup');
    if (!g) return;
    const key = g.dataset.key;
    if (key === 'channel' || key === 'bright') {
      state.hot = key;
      refresh();
    }
  });
  byteGroups.addEventListener('mouseleave', () => setHot(null));

  /* presets from hint chips */
  $$('.hint-chip').forEach((chip) => {
    chip.addEventListener('click', () => {
      const [ch, br] = chip.dataset.preset.split(',').map(Number);
      inCh.value = String(ch);
      inBr.value = String(br);
      state.cmd = 'bright';
      // select first result
      $$('.res-item').forEach((r) => r.classList.toggle('selected', r.dataset.cmd === 'bright'));
      $('#cmdTitle').textContent = '设置通道亮度';
      refresh();
    });
  });

  /* result selection (demo data) */
  $$('.res-item').forEach((item) => {
    item.addEventListener('click', () => {
      $$('.res-item').forEach((r) => r.classList.toggle('selected', r === item));
      const cmd = item.dataset.cmd;
      state.cmd = cmd;
      const title = $('#cmdTitle');
      const desc = $('#cmdDesc');
      const form = $('#paramForm');
      const anatomy = $('#anatomy');

      if (cmd === 'bright') {
        title.textContent = '设置通道亮度';
        desc.innerHTML =
          '设置指定通道的亮度值。范围 <strong>0~999</strong>（16 位大端），修改立即生效、断电不保存。建议设置后读回确认。<strong>不会向设备发送</strong>——只生成可复制的帧。';
        form.style.display = '';
        anatomy.style.display = '';
      } else if (cmd === 'readbright') {
        title.textContent = '读取通道亮度';
        desc.innerHTML =
          '读取指定通道当前亮度。<strong>只读 · L0</strong>。回复为 16 位大端亮度值。本工具只组帧，不发送。';
        form.style.display = '';
        anatomy.style.display = '';
      } else {
        title.textContent = '读取设备 IP';
        desc.innerHTML =
          '查询控制器当前 IPv4 地址。<strong>只读 · L0</strong>，无业务参数。本工具只组帧，不发送。';
        form.style.display = 'none';
        anatomy.style.display = '';
      }

      // rebuild frame for non-bright as simple demo
      if (cmd !== 'bright') {
        byteGroups.innerHTML =
          '<div class="bgroup is-fixed"><div class="glabel">帧头</div><div class="bracket"></div><div class="byte-row"><div class="byte fixed">CA</div></div></div>' +
          '<div class="bgroup is-fixed"><div class="glabel">命令</div><div class="bracket"></div><div class="byte-row"><div class="byte fixed">' +
          (cmd === 'readbright' ? '02' : '0F') +
          '</div></div></div>' +
          '<div class="bgroup is-comp"><div class="glabel">BCC</div><div class="bracket"></div><div class="byte-row"><div class="byte comp">' +
          (cmd === 'readbright' ? 'C8' : 'C5') +
          '</div></div></div>';
        hexLine.textContent = cmd === 'readbright' ? 'CA 02 C8' : 'CA 0F C5';
        readable.innerHTML =
          cmd === 'readbright'
            ? '帧头 <b>CA</b> · 命令 <b>02</b>（读亮度）· 校验 <b>BCC=C8</b>'
            : '帧头 <b>CA</b> · 命令 <b>0F</b>（读 IP）· 校验 <b>BCC=C5</b>';
        toastCode.textContent = hexLine.textContent;
      } else {
        refresh();
      }
    });
  });

  /* copy actions */
  function flashCopy(btn) {
    const old = btn.textContent;
    btn.classList.add('done');
    btn.textContent = '✓ 已复制';
    toast.classList.add('show');
    clearTimeout(flashCopy._t);
    flashCopy._t = setTimeout(() => {
      btn.classList.remove('done');
      btn.textContent = old;
      toast.classList.remove('show');
    }, 1200);
  }

  function copyText(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).catch(() => {});
    } else {
      const ta = document.createElement('textarea');
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      try {
        document.execCommand('copy');
      } catch (_) {}
      document.body.removeChild(ta);
    }
  }

  btnHex.addEventListener('click', () => {
    const t = hexLine.textContent.trim();
    copyText(t);
    toastCode.textContent = t;
    flashCopy(btnHex);
  });

  btnAscii.addEventListener('click', () => {
    // demo: only meaningful for ASCII protocols; show hex-as-text notice
    const t = hexLine.textContent.trim();
    copyText(t);
    toastCode.textContent = t + ' (HEX)';
    flashCopy(btnAscii);
  });

  // init
  refresh();
})();
