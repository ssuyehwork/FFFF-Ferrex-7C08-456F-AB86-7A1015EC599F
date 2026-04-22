<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Ferrex</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Barlow+Condensed:wght@300;400;500;600;700&family=JetBrains+Mono:wght@300;400;500&display=swap" rel="stylesheet">
<style>
/* ── Reset & Root ─────────────────────────────────────────────── */
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

:root {
  --bg:        #07090b;
  --bg1:       #0d1014;
  --bg2:       #111519;
  --bg3:       #161b20;
  --border:    #1e252c;
  --border2:   #252e37;
  --accent:    #FF8C00;
  --accent2:   #c96e00;
  --accent-lo: rgba(255,140,0,0.08);
  --accent-md: rgba(255,140,0,0.15);
  --text:      #c8d4dc;
  --text2:     #7a8f9e;
  --text3:     #3d5060;
  --success:   #2ecc71;
  --danger:    #e74c3c;
  --mono:      'JetBrains Mono', monospace;
  --cond:      'Barlow Condensed', sans-serif;
}

html, body {
  width: 100%; height: 100%;
  background: var(--bg);
  color: var(--text);
  font-family: var(--mono);
  font-size: 13px;
  overflow: hidden;
  user-select: none;
  cursor: default;
}

/* ── Scanline overlay ─────────────────────────────────────────── */
body::before {
  content: '';
  position: fixed; inset: 0;
  background: repeating-linear-gradient(
    0deg,
    transparent,
    transparent 2px,
    rgba(0,0,0,0.06) 2px,
    rgba(0,0,0,0.06) 4px
  );
  pointer-events: none;
  z-index: 9999;
}

/* ── Layout shell ─────────────────────────────────────────────── */
#app {
  display: grid;
  grid-template-rows: 44px 1px auto 1px auto 1px 1fr 1px 26px;
  height: 100vh;
  width: 100%;
}

/* ── Dividers ─────────────────────────────────────────────────── */
.div { background: var(--border); }

/* ── Titlebar ─────────────────────────────────────────────────── */
#titlebar {
  display: flex;
  align-items: center;
  padding: 0 16px;
  background: var(--bg1);
  gap: 14px;
  -webkit-app-region: drag;
}

.logo-mark {
  display: flex;
  align-items: center;
  gap: 8px;
  -webkit-app-region: no-drag;
}

.logo-hex {
  width: 22px; height: 22px;
  color: var(--accent);
}

.logo-text {
  font-family: var(--cond);
  font-weight: 700;
  font-size: 18px;
  letter-spacing: 0.12em;
  color: var(--accent);
  text-transform: uppercase;
}

.title-spacer { flex: 1; }

.title-badge {
  font-family: var(--cond);
  font-size: 11px;
  font-weight: 500;
  letter-spacing: 0.08em;
  color: var(--text3);
  text-transform: uppercase;
  padding: 3px 8px;
  border: 1px solid var(--border2);
  background: var(--bg2);
}

.index-status {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 11px;
  color: var(--text3);
  font-family: var(--cond);
  letter-spacing: 0.05em;
}

.status-dot {
  width: 6px; height: 6px;
  border-radius: 50%;
  background: var(--success);
  box-shadow: 0 0 6px var(--success);
}

.wm-btn {
  width: 28px; height: 28px;
  display: flex; align-items: center; justify-content: center;
  background: transparent;
  border: none;
  color: var(--text3);
  cursor: pointer;
  transition: background 0.15s, color 0.15s;
  -webkit-app-region: no-drag;
}
.wm-btn:hover { background: var(--bg3); color: var(--text); }
.wm-btn.close:hover { background: var(--danger); color: #fff; }

/* ── Drive selector bar ───────────────────────────────────────── */
#drives {
  display: flex;
  align-items: center;
  padding: 0 16px;
  background: var(--bg2);
  gap: 6px;
  height: 40px;
}

.drive-label {
  font-family: var(--cond);
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.14em;
  color: var(--text3);
  text-transform: uppercase;
  margin-right: 4px;
}

.drive-chip {
  position: relative;
  display: flex;
  align-items: center;
  gap: 5px;
  padding: 4px 10px;
  border: 1px solid var(--border2);
  background: var(--bg3);
  color: var(--text2);
  font-family: var(--cond);
  font-size: 12px;
  font-weight: 600;
  letter-spacing: 0.08em;
  cursor: pointer;
  transition: all 0.12s;
  white-space: nowrap;
}

.drive-chip:hover {
  border-color: var(--accent2);
  color: var(--text);
}

.drive-chip.active {
  border-color: var(--accent);
  background: var(--accent-lo);
  color: var(--accent);
}

.drive-chip.active .chip-dot {
  background: var(--accent);
  box-shadow: 0 0 4px var(--accent);
}

.chip-dot {
  width: 5px; height: 5px;
  border-radius: 50%;
  background: var(--text3);
  transition: all 0.12s;
}

.chip-count {
  font-size: 9px;
  font-weight: 400;
  color: inherit;
  opacity: 0.6;
  margin-left: 1px;
}

.drives-spacer { flex: 1; }

.all-btn {
  font-family: var(--cond);
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.1em;
  color: var(--text3);
  text-transform: uppercase;
  padding: 4px 10px;
  border: 1px solid var(--border);
  background: transparent;
  cursor: pointer;
  transition: all 0.12s;
}
.all-btn:hover { color: var(--accent); border-color: var(--accent); }

/* ── Search bar ───────────────────────────────────────────────── */
#searchbar {
  display: flex;
  align-items: center;
  padding: 0 16px;
  background: var(--bg1);
  gap: 0;
  height: 46px;
}

.search-icon-wrap {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 36px; height: 36px;
  border: 1px solid var(--border2);
  border-right: none;
  background: var(--bg2);
  color: var(--text3);
  flex-shrink: 0;
}

.search-input-wrap {
  flex: 1;
  position: relative;
}

.search-input {
  width: 100%;
  height: 36px;
  background: var(--bg2);
  border: 1px solid var(--border2);
  border-right: none;
  color: var(--text);
  font-family: var(--mono);
  font-size: 13px;
  padding: 0 12px;
  outline: none;
  transition: border-color 0.15s, background 0.15s;
  caret-color: var(--accent);
}

.search-input:focus {
  border-color: var(--accent);
  background: var(--bg3);
}

.search-input::placeholder { color: var(--text3); }

.ext-divider {
  display: flex;
  align-items: center;
  justify-content: center;
  height: 36px;
  padding: 0 10px;
  background: var(--bg3);
  border-top: 1px solid var(--border2);
  border-bottom: 1px solid var(--border2);
  color: var(--accent);
  font-family: var(--mono);
  font-size: 14px;
  font-weight: 500;
  flex-shrink: 0;
  border-left: 1px solid var(--border2);
}

.ext-input {
  width: 100px;
  height: 36px;
  background: var(--bg2);
  border: 1px solid var(--border2);
  border-left: none;
  color: var(--text);
  font-family: var(--mono);
  font-size: 13px;
  padding: 0 10px;
  outline: none;
  transition: border-color 0.15s, background 0.15s;
  caret-color: var(--accent);
}

.ext-input:focus {
  border-color: var(--accent);
  background: var(--bg3);
}

.ext-input::placeholder { color: var(--text3); }

.search-btn {
  height: 36px;
  padding: 0 18px;
  background: var(--accent);
  border: 1px solid var(--accent);
  color: #000;
  font-family: var(--cond);
  font-size: 13px;
  font-weight: 700;
  letter-spacing: 0.12em;
  text-transform: uppercase;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 7px;
  flex-shrink: 0;
  margin-left: 10px;
  transition: background 0.12s;
}
.search-btn:hover { background: var(--accent2); border-color: var(--accent2); }
.search-btn svg { color: #000; }

/* ── Column header ────────────────────────────────────────────── */
#col-header {
  display: grid;
  grid-template-columns: 28px 1fr 2fr 80px 130px;
  align-items: center;
  padding: 0 16px;
  background: var(--bg2);
  height: 30px;
  border-left: 3px solid transparent;
}

.col-h {
  font-family: var(--cond);
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.12em;
  color: var(--text3);
  text-transform: uppercase;
  display: flex;
  align-items: center;
  gap: 4px;
  cursor: pointer;
  transition: color 0.12s;
}
.col-h:hover { color: var(--text2); }
.col-h.sort-asc svg { transform: rotate(0deg); color: var(--accent); }
.col-h.sort-desc svg { transform: rotate(180deg); color: var(--accent); }

/* ── Results list ─────────────────────────────────────────────── */
#results {
  overflow-y: auto;
  background: var(--bg);
}

/* Custom scrollbar */
#results::-webkit-scrollbar { width: 6px; }
#results::-webkit-scrollbar-track { background: var(--bg); }
#results::-webkit-scrollbar-thumb { background: var(--border2); }
#results::-webkit-scrollbar-thumb:hover { background: var(--text3); }

.result-row {
  display: grid;
  grid-template-columns: 28px 1fr 2fr 80px 130px;
  align-items: center;
  padding: 0 16px;
  height: 30px;
  border-left: 3px solid transparent;
  border-bottom: 1px solid rgba(30,37,44,0.5);
  transition: background 0.08s, border-left-color 0.08s;
  cursor: default;
}

.result-row:hover {
  background: var(--bg2);
  border-left-color: var(--accent);
}

.result-row:nth-child(even) { background: rgba(13,16,20,0.4); }
.result-row:nth-child(even):hover { background: var(--bg2); }

.cell-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--text3);
}

.cell-icon.is-dir { color: var(--accent2); }

.cell-name {
  font-family: var(--mono);
  font-size: 12.5px;
  color: var(--text);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  padding-right: 12px;
}

.cell-name .match { color: var(--accent); }

.cell-path {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--text3);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  padding-right: 12px;
}

.cell-size {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--text2);
  text-align: right;
  padding-right: 16px;
}

.cell-date {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--text3);
}

/* drive tag inline */
.drive-tag {
  display: inline-block;
  font-size: 9px;
  font-family: var(--cond);
  font-weight: 700;
  letter-spacing: 0.06em;
  padding: 1px 4px;
  border: 1px solid var(--border2);
  color: var(--text3);
  margin-right: 4px;
  vertical-align: middle;
  flex-shrink: 0;
}

/* ── Statusbar ────────────────────────────────────────────────── */
#statusbar {
  display: flex;
  align-items: center;
  padding: 0 16px;
  background: var(--bg1);
  gap: 20px;
}

.stat-item {
  display: flex;
  align-items: center;
  gap: 5px;
  font-family: var(--cond);
  font-size: 10px;
  letter-spacing: 0.07em;
  color: var(--text3);
  text-transform: uppercase;
}

.stat-item span { color: var(--text2); font-weight: 600; }
.stat-spacer { flex: 1; }

.stat-accent { color: var(--accent) !important; }

/* ── Empty state ──────────────────────────────────────────────── */
#empty-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  gap: 12px;
  color: var(--text3);
}

.empty-icon { color: var(--border2); }
.empty-text {
  font-family: var(--cond);
  font-size: 14px;
  letter-spacing: 0.1em;
  text-transform: uppercase;
}
.empty-sub {
  font-size: 11px;
  color: var(--text3);
  opacity: 0.6;
}

/* ── Animations ───────────────────────────────────────────────── */
@keyframes fadeSlideIn {
  from { opacity: 0; transform: translateY(4px); }
  to   { opacity: 1; transform: translateY(0); }
}

.result-row {
  animation: fadeSlideIn 0.12s ease both;
}

#searchbar { animation: fadeSlideIn 0.2s 0.05s ease both; }
#drives    { animation: fadeSlideIn 0.2s 0.1s ease both; }
</style>
</head>
<body>
<div id="app">

  <!-- ── Titlebar ── -->
  <div id="titlebar">
    <div class="logo-mark">
      <!-- SVG cached as #ico-hex -->
      <svg class="logo-hex" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5">
        <polygon points="12,2 21,7 21,17 12,22 3,17 3,7"/>
        <line x1="12" y1="2" x2="12" y2="22"/>
        <line x1="3" y1="7" x2="21" y2="17"/>
        <line x1="21" y1="7" x2="3" y2="17"/>
      </svg>
      <span class="logo-text">Ferrex</span>
    </div>

    <div class="title-badge">NTFS Indexer</div>

    <div class="title-spacer"></div>

    <div class="index-status">
      <div class="status-dot"></div>
      <span>索引就绪 &mdash; 4,269,484 条记录</span>
    </div>

    <div style="width:12px"></div>

    <button class="wm-btn" title="最小化">
      <svg width="12" height="1" viewBox="0 0 12 1" fill="currentColor"><rect width="12" height="1"/></svg>
    </button>
    <button class="wm-btn" title="最大化">
      <svg width="11" height="11" viewBox="0 0 11 11" fill="none" stroke="currentColor" stroke-width="1.2"><rect x="0.6" y="0.6" width="9.8" height="9.8"/></svg>
    </button>
    <button class="wm-btn close" title="关闭">
      <svg width="11" height="11" viewBox="0 0 11 11" fill="none" stroke="currentColor" stroke-width="1.4">
        <line x1="1" y1="1" x2="10" y2="10"/>
        <line x1="10" y1="1" x2="1" y2="10"/>
      </svg>
    </button>
  </div>
  <div class="div"></div>

  <!-- ── Drive selector ── -->
  <div id="drives">
    <span class="drive-label">盘符</span>

    <div class="drive-chip active" data-drive="C" onclick="toggleDrive(this)">
      <div class="chip-dot"></div>
      C:
      <span class="chip-count">2.3M</span>
    </div>
    <div class="drive-chip active" data-drive="G" onclick="toggleDrive(this)">
      <div class="chip-dot"></div>
      G:
      <span class="chip-count">1.1M</span>
    </div>
    <div class="drive-chip" data-drive="H" onclick="toggleDrive(this)">
      <div class="chip-dot"></div>
      H:
      <span class="chip-count">676K</span>
    </div>
    <div class="drive-chip" data-drive="I" onclick="toggleDrive(this)">
      <div class="chip-dot"></div>
      I:
      <span class="chip-count">20K</span>
    </div>
    <div class="drive-chip" data-drive="Z" onclick="toggleDrive(this)">
      <div class="chip-dot"></div>
      Z:
      <span class="chip-count">95K</span>
    </div>

    <div class="drives-spacer"></div>
    <button class="all-btn" onclick="toggleAll()">全选 / 全清</button>
  </div>
  <div class="div"></div>

  <!-- ── Search bar ── -->
  <div id="searchbar">
    <div class="search-icon-wrap">
      <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">
        <circle cx="11" cy="11" r="7"/>
        <line x1="16.5" y1="16.5" x2="22" y2="22"/>
      </svg>
    </div>
    <div class="search-input-wrap">
      <input class="search-input" id="searchInput"
             type="text" placeholder="文件名 / 关键词..."
             oninput="onSearch()" autocomplete="off" spellcheck="false">
    </div>
    <div class="ext-divider">.</div>
    <input class="ext-input" id="extInput"
           type="text" placeholder="扩展名"
           oninput="onSearch()" autocomplete="off" spellcheck="false">
    <button class="search-btn" onclick="onSearch()">
      <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2">
        <circle cx="11" cy="11" r="7"/>
        <line x1="16.5" y1="16.5" x2="22" y2="22"/>
      </svg>
      搜索
    </button>
  </div>
  <div class="div"></div>

  <!-- ── Column header ── -->
  <div id="col-header">
    <div></div>
    <div class="col-h sort-asc" onclick="sortBy('name')">
      名称
      <svg width="8" height="8" viewBox="0 0 8 8" fill="currentColor">
        <polygon points="4,1 7,6 1,6"/>
      </svg>
    </div>
    <div class="col-h" onclick="sortBy('path')">路径</div>
    <div class="col-h" style="justify-content:flex-end; padding-right:16px" onclick="sortBy('size')">大小</div>
    <div class="col-h" onclick="sortBy('date')">修改时间</div>
  </div>
  <div class="div"></div>

  <!-- ── Results ── -->
  <div id="results"></div>

  <div class="div"></div>

  <!-- ── Statusbar ── -->
  <div id="statusbar">
    <div class="stat-item">结果 <span id="stat-count">—</span></div>
    <div class="stat-item">耗时 <span id="stat-time">—</span></div>
    <div class="stat-item">盘符 <span id="stat-drives">C: G:</span></div>
    <div class="stat-spacer"></div>
    <div class="stat-item">上次索引 <span>2026-04-22 17:41</span></div>
    <div class="stat-item stat-accent">FERREX v0.1.0</div>
  </div>

</div>

<!-- ── SVG Icon sprite cache ── -->
<svg style="display:none" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <symbol id="ico-file" viewBox="0 0 16 16">
      <path d="M9 1H4a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V5z"
            fill="none" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"/>
      <polyline points="9,1 9,5 13,5" fill="none" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"/>
    </symbol>
    <symbol id="ico-dir" viewBox="0 0 16 16">
      <path d="M1 4a1 1 0 0 1 1-1h4l2 2h6a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1H2a1 1 0 0 1-1-1z"
            fill="none" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"/>
    </symbol>
    <symbol id="ico-exe" viewBox="0 0 16 16">
      <rect x="2" y="2" width="12" height="12" rx="1" fill="none" stroke="currentColor" stroke-width="1.2"/>
      <path d="M5 8l2-2 2 2 2-2" fill="none" stroke="currentColor" stroke-width="1.2" stroke-linecap="round"/>
      <line x1="5" y1="11" x2="11" y2="11" stroke="currentColor" stroke-width="1.2" stroke-linecap="round"/>
    </symbol>
    <symbol id="ico-img" viewBox="0 0 16 16">
      <rect x="1" y="3" width="14" height="10" rx="1" fill="none" stroke="currentColor" stroke-width="1.2"/>
      <circle cx="5.5" cy="6.5" r="1.2" fill="currentColor"/>
      <polyline points="1,11 5,7.5 8,10 11,7 15,11" fill="none" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"/>
    </symbol>
    <symbol id="ico-doc" viewBox="0 0 16 16">
      <path d="M9 1H4a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V5z"
            fill="none" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"/>
      <polyline points="9,1 9,5 13,5" fill="none" stroke="currentColor" stroke-width="1.2"/>
      <line x1="5" y1="8" x2="11" y2="8" stroke="currentColor" stroke-width="1.1" stroke-linecap="round"/>
      <line x1="5" y1="10.5" x2="11" y2="10.5" stroke="currentColor" stroke-width="1.1" stroke-linecap="round"/>
      <line x1="5" y1="13" x2="8" y2="13" stroke="currentColor" stroke-width="1.1" stroke-linecap="round"/>
    </symbol>
    <symbol id="ico-sort" viewBox="0 0 8 8">
      <polygon points="4,1 7,6 1,6"/>
    </symbol>
  </defs>
</svg>

<script>
/* ── Icon cache pool ───────────────────────────────────────────── */
const IconCache = (() => {
  const pool = new Map();
  const EXT_MAP = {
    exe: 'ico-exe', dll: 'ico-exe', msi: 'ico-exe', bat: 'ico-exe', cmd: 'ico-exe',
    png: 'ico-img', jpg: 'ico-img', jpeg: 'ico-img', gif: 'ico-img', webp: 'ico-img', svg: 'ico-img', bmp: 'ico-img', ico: 'ico-img',
    doc: 'ico-doc', docx: 'ico-doc', pdf: 'ico-doc', txt: 'ico-doc', md: 'ico-doc', rtf: 'ico-doc',
    xls: 'ico-doc', xlsx: 'ico-doc', ppt: 'ico-doc', pptx: 'ico-doc',
  };

  function getSymbol(name, isDir) {
    if (isDir) return 'ico-dir';
    const ext = name.split('.').pop().toLowerCase();
    return EXT_MAP[ext] || 'ico-file';
  }

  function get(name, isDir) {
    const sym = getSymbol(name, isDir);
    if (pool.has(sym)) return pool.get(sym);
    const el = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    el.setAttribute('width', '14');
    el.setAttribute('height', '14');
    const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    use.setAttribute('href', '#' + sym);
    el.appendChild(use);
    pool.set(sym, el);
    return el;
  }

  // Pre-warm cache
  function warmUp() {
    ['ico-file','ico-dir','ico-exe','ico-img','ico-doc'].forEach(sym => {
      const el = document.createElementNS('http://www.w3.org/2000/svg','svg');
      el.setAttribute('width','14'); el.setAttribute('height','14');
      const use = document.createElementNS('http://www.w3.org/2000/svg','use');
      use.setAttribute('href','#'+sym);
      el.appendChild(use);
      pool.set(sym, el);
    });
  }

  return { get, warmUp };
})();

IconCache.warmUp();

/* ── Mock data ─────────────────────────────────────────────────── */
const MOCK_DATA = [
  { name:'ferrex_gui.exe',      path:'G:\\C++\\Ferrex\\Ferrex\\ferrex\\target\\release', size:'8.42 MB',  date:'2026-04-22 17:41', drive:'G', isDir:false },
  { name:'ferrex_cli.exe',      path:'G:\\C++\\Ferrex\\Ferrex\\ferrex\\target\\release', size:'4.10 MB',  date:'2026-04-22 17:41', drive:'G', isDir:false },
  { name:'Cargo.toml',          path:'G:\\C++\\Ferrex\\Ferrex\\ferrex',                  size:'712 B',    date:'2026-04-22 16:03', drive:'G', isDir:false },
  { name:'main.rs',             path:'G:\\C++\\Ferrex\\Ferrex\\ferrex\\ferrex_gui\\src', size:'9.87 KB',  date:'2026-04-22 15:58', drive:'G', isDir:false },
  { name:'c_drive.idx',         path:'G:\\C++\\Ferrex\\Ferrex\\ferrex',                  size:'155.3 MB', date:'2026-04-22 17:41', drive:'G', isDir:false },
  { name:'Windows',             path:'C:\\',                                             size:'—',        date:'2025-11-14 09:00', drive:'C', isDir:true  },
  { name:'System32',            path:'C:\\Windows',                                      size:'—',        date:'2026-04-20 03:11', drive:'C', isDir:true  },
  { name:'explorer.exe',        path:'C:\\Windows',                                      size:'5.07 MB',  date:'2025-11-14 08:15', drive:'C', isDir:false },
  { name:'notepad.exe',         path:'C:\\Windows\\System32',                            size:'238 KB',   date:'2025-11-14 08:15', drive:'C', isDir:false },
  { name:'python.exe',          path:'C:\\Program Files\\Python312',                     size:'103 KB',   date:'2024-09-07 12:00', drive:'C', isDir:false },
  { name:'rustc.exe',           path:'C:\\Users\\fachu\\.cargo\\bin',                    size:'14.2 MB',  date:'2025-12-01 08:00', drive:'C', isDir:false },
  { name:'cargo.exe',           path:'C:\\Users\\fachu\\.cargo\\bin',                    size:'2.88 MB',  date:'2025-12-01 08:00', drive:'C', isDir:false },
  { name:'index.html',          path:'C:\\Users\\fachu\\Desktop\\project',               size:'44 KB',    date:'2026-04-10 11:22', drive:'C', isDir:false },
  { name:'README.md',           path:'G:\\C++\\Ferrex\\Ferrex\\ferrex',                  size:'3.12 KB',  date:'2026-04-22 14:00', drive:'G', isDir:false },
  { name:'search.rs',           path:'G:\\C++\\Ferrex\\Ferrex\\ferrex\\crates\\search\\src', size:'6.44 KB', date:'2026-04-22 15:10', drive:'G', isDir:false },
  { name:'storage.rs',          path:'G:\\C++\\Ferrex\\Ferrex\\ferrex\\crates\\storage\\src', size:'11.2 KB', date:'2026-04-22 15:30', drive:'G', isDir:false },
  { name:'screenshot.png',      path:'C:\\Users\\fachu\\Pictures',                       size:'2.11 MB',  date:'2026-04-22 17:00', drive:'C', isDir:false },
  { name:'background.jpg',      path:'C:\\Users\\fachu\\Pictures\\Wallpapers',           size:'8.77 MB',  date:'2026-01-05 09:14', drive:'C', isDir:false },
  { name:'report_Q1.xlsx',      path:'C:\\Users\\fachu\\Documents',                      size:'1.03 MB',  date:'2026-04-01 18:00', drive:'C', isDir:false },
  { name:'ferrex_builder.py',   path:'G:\\C++\\Ferrex\\Ferrex',                          size:'14.6 KB',  date:'2026-04-22 18:00', drive:'G', isDir:false },
];

/* ── State ─────────────────────────────────────────────────────── */
let activeDrives = new Set(['C','G']);
let sortKey = 'name';
let sortDir = 1;
let lastQuery = '';
let lastExt = '';

/* ── Drive toggle ──────────────────────────────────────────────── */
function toggleDrive(el) {
  const d = el.dataset.drive;
  if (activeDrives.has(d)) {
    if (activeDrives.size === 1) return; // keep at least one
    activeDrives.delete(d);
    el.classList.remove('active');
  } else {
    activeDrives.add(d);
    el.classList.add('active');
  }
  updateDriveStat();
  onSearch();
}

function toggleAll() {
  const chips = document.querySelectorAll('.drive-chip');
  const allActive = chips.length === activeDrives.size;
  if (allActive) {
    activeDrives = new Set([chips[0].dataset.drive]);
    chips.forEach((c,i) => { if(i===0) c.classList.add('active'); else c.classList.remove('active'); });
  } else {
    chips.forEach(c => { activeDrives.add(c.dataset.drive); c.classList.add('active'); });
  }
  updateDriveStat();
  onSearch();
}

function updateDriveStat() {
  document.getElementById('stat-drives').textContent =
    [...activeDrives].sort().map(d => d + ':').join(' ') || '—';
}

/* ── Sort ──────────────────────────────────────────────────────── */
function sortBy(key) {
  if (sortKey === key) sortDir *= -1;
  else { sortKey = key; sortDir = 1; }
  document.querySelectorAll('.col-h').forEach(h => {
    h.classList.remove('sort-asc','sort-desc');
  });
  const idx = ['','name','path','size','date'].indexOf(key);
  const headers = document.querySelectorAll('.col-h');
  if (idx > 0 && headers[idx-1]) {
    headers[idx-1].classList.add(sortDir === 1 ? 'sort-asc' : 'sort-desc');
  }
  onSearch();
}

/* ── Search & render ───────────────────────────────────────────── */
function highlight(text, query) {
  if (!query) return text;
  const idx = text.toLowerCase().indexOf(query.toLowerCase());
  if (idx === -1) return text;
  return text.slice(0,idx)
    + '<span class="match">' + text.slice(idx, idx+query.length) + '</span>'
    + text.slice(idx+query.length);
}

function onSearch() {
  const t0 = performance.now();
  const q   = document.getElementById('searchInput').value.trim();
  const ext = document.getElementById('extInput').value.trim().toLowerCase().replace(/^\./,'');
  lastQuery = q; lastExt = ext;

  let rows = MOCK_DATA.filter(r => {
    if (!activeDrives.has(r.drive)) return false;
    if (q && !r.name.toLowerCase().includes(q.toLowerCase()) &&
             !r.path.toLowerCase().includes(q.toLowerCase())) return false;
    if (ext && !r.name.toLowerCase().endsWith('.' + ext)) return false;
    return true;
  });

  rows.sort((a,b) => {
    let av = a[sortKey] || '', bv = b[sortKey] || '';
    if (sortKey === 'size') {
      av = parseFloat(av) || 0; bv = parseFloat(bv) || 0;
      return (av - bv) * sortDir;
    }
    return av.localeCompare(bv) * sortDir;
  });

  const elapsed = (performance.now() - t0).toFixed(1);
  renderResults(rows, q);
  document.getElementById('stat-count').textContent = rows.length.toLocaleString();
  document.getElementById('stat-time').textContent  = elapsed + ' ms';
}

function renderResults(rows, query) {
  const container = document.getElementById('results');
  container.innerHTML = '';

  if (!rows.length) {
    const es = document.createElement('div');
    es.id = 'empty-state';
    es.innerHTML = `
      <svg class="empty-icon" width="36" height="36" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1">
        <circle cx="11" cy="11" r="8"/>
        <line x1="16.5" y1="16.5" x2="22" y2="22"/>
        <line x1="8" y1="11" x2="14" y2="11"/>
      </svg>
      <div class="empty-text">无匹配结果</div>
      <div class="empty-sub">尝试更换关键词或扩大盘符范围</div>`;
    container.appendChild(es);
    return;
  }

  const frag = document.createDocumentFragment();
  rows.forEach((r, i) => {
    const row = document.createElement('div');
    row.className = 'result-row';
    row.style.animationDelay = Math.min(i * 10, 80) + 'ms';

    // icon cell
    const iconCell = document.createElement('div');
    iconCell.className = 'cell-icon' + (r.isDir ? ' is-dir' : '');
    const iconSvg = IconCache.get(r.name, r.isDir).cloneNode(true);
    iconCell.appendChild(iconSvg);

    // name cell
    const nameCell = document.createElement('div');
    nameCell.className = 'cell-name';
    const driveTag = `<span class="drive-tag">${r.drive}:</span>`;
    nameCell.innerHTML = driveTag + highlight(r.name, query);

    // path cell
    const pathCell = document.createElement('div');
    pathCell.className = 'cell-path';
    pathCell.title = r.path;
    pathCell.textContent = r.path;

    // size cell
    const sizeCell = document.createElement('div');
    sizeCell.className = 'cell-size';
    sizeCell.textContent = r.isDir ? '—' : r.size;

    // date cell
    const dateCell = document.createElement('div');
    dateCell.className = 'cell-date';
    dateCell.textContent = r.date;

    row.appendChild(iconCell);
    row.appendChild(nameCell);
    row.appendChild(pathCell);
    row.appendChild(sizeCell);
    row.appendChild(dateCell);
    frag.appendChild(row);
  });
  container.appendChild(frag);
}

/* ── Init ──────────────────────────────────────────────────────── */
updateDriveStat();
onSearch();
</script>
</body>
</html>