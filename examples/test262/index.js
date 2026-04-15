import fs from 'fs';
import { join } from 'path';
import { spawnSync } from 'child_process';

const basePath = import.meta.dirname;
const indexPath = join(basePath, 'ant.json');

const data = JSON.parse(fs.readFileSync(indexPath, 'utf8'));
const entries = Object.entries(data.passes).sort();

const stats = {
  total: entries.length,
  passed: entries.filter(([_, v]) => v).length,
  failed: entries.filter(([_, v]) => !v).length,
  get rate() {
    return ((this.passed / this.total) * 100).toFixed(2);
  }
};

const ESC = '\x1b';
const CSI = `${ESC}[`;

const term = {
  hideCursor: `${CSI}?25l`,
  showCursor: `${CSI}?25h`,
  altScreenOn: `${CSI}?1049h`,
  altScreenOff: `${CSI}?1049l`,
  syncStart: `${CSI}?2026h`,
  syncEnd: `${CSI}?2026l`,
  home: `${CSI}H`,
  clearScreen: `${CSI}2J`,
  reset: `${CSI}0m`,
  bold: `${CSI}1m`,
  dim: `${CSI}2m`,
  fg: n => `${CSI}38;5;${n}m`,
  bg: n => `${CSI}48;5;${n}m`
};

const c = {
  reset: term.reset,
  bold: term.bold,
  dim: term.dim,
  red: term.fg(196),
  green: term.fg(82),
  yellow: term.fg(226),
  blue: term.fg(39),
  cyan: term.fg(51),
  gray: term.fg(245),
  bgSelect: term.bg(24)
};

let state = {
  mode: 'browse',
  index: 0,
  filter: '',
  filtered: entries,
  memoryCache: null,
  memoryOffset: 0,
  memoryStatus: '',
  memoryStatusAt: 0,
  fullClearNextRender: false,
  searchMode: false,
  searchQuery: ''
};

let fps = {
  frames: 0,
  lastTime: performance.now(),
  current: 0,
  update() {
    this.frames++;
    const now = performance.now();
    const delta = now - this.lastTime;
    if (delta >= 1000) {
      this.current = Math.round((this.frames * 1000) / delta);
      this.frames = 0;
      this.lastTime = now;
    }
  }
};

function fmt(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
  return (bytes / 1024 / 1024).toFixed(2) + ' MB';
}

function getRows() {
  return process.stdout.rows || 24;
}
function getCols() {
  return process.stdout.columns || 80;
}

function pad(str, len) {
  const visible = str.replace(/\x1b\[[0-9;]*m/g, '');
  return str + ' '.repeat(Math.max(0, len - visible.length));
}

function truncate(str, len) {
  if (str.length <= len) return str;
  return str.slice(0, len - 1) + '…';
}

const byteStatSuffixes = new Set([
  'used',
  'capacity',
  'totalUsed',
  'totalCapacity',
  'objects',
  'overflow',
  'extraSlots',
  'promises',
  'proxies',
  'exotic',
  'arrays',
  'shapes',
  'closures',
  'upvalues',
  'propRefs',
  'total',
  'buffers',
  'code',
  'bytes',
  'cstack',
  'rss',
  'residentSize',
  'physFootprint',
  'virtualSize'
]);

function formatStatValue(path, value, styled = true) {
  if (typeof value === 'number') {
    const key = path.split('.').pop();
    if (byteStatSuffixes.has(key)) {
      return styled ? `${fmt(value)} ${c.dim}(${value})${c.reset}` : `${fmt(value)} (${value})`;
    }
  }
  return String(value);
}

function flattenStats(value, prefix = '', out = [], styled = true) {
  if (value && typeof value === 'object') {
    for (const [key, child] of Object.entries(value)) {
      flattenStats(child, prefix ? `${prefix}.${key}` : key, out, styled);
    }
    return out;
  }

  if (styled) {
    out.push(`${c.gray}${prefix}${c.reset}: ${c.bold}${formatStatValue(prefix, value, true)}${c.reset}`);
  } else {
    out.push(`${prefix}: ${formatStatValue(prefix, value, false)}`);
  }
  return out;
}

function collectMemorySnapshot() {
  const mem = Ant.stats();
  const gcProfile = Ant.raw && typeof Ant.raw.gcMarkProfile === 'function'
    ? Ant.raw.gcMarkProfile()
    : null;

  return { mem, gcProfile };
}

function buildMemoryLinesFromSnapshot(snapshot, styled = true) {
  const { mem, gcProfile } = snapshot;
  const lines = styled
    ? [`${c.cyan}Ant.stats()${c.reset}`, `${c.dim}Raw runtime stats dump${c.reset}`, '']
    : ['Ant.stats()', 'Raw runtime stats dump', ''];
  lines.push(...flattenStats(mem, '', [], styled));

  if (gcProfile) {
    lines.push('');
    lines.push(styled ? `${c.cyan}Ant.raw.gcMarkProfile()${c.reset}` : 'Ant.raw.gcMarkProfile()');
    lines.push(styled ? `${c.dim}Extra GC mark profiler stats${c.reset}` : 'Extra GC mark profiler stats');
    lines.push('');
    lines.push(...flattenStats(gcProfile, 'gcMarkProfile', [], styled));
  }

  return lines;
}

function rebuildMemoryDisplayCache() {
  if (!state.memoryCache) return;

  const cols = getCols();
  state.memoryCache.cols = cols;
  state.memoryCache.displayLines = state.memoryCache.styledLines.map(line => pad(line, cols));
}

function refreshMemoryCache() {
  const snapshot = collectMemorySnapshot();
  state.memoryCache = {
    snapshot,
    styledLines: buildMemoryLinesFromSnapshot(snapshot, true),
    plainLines: buildMemoryLinesFromSnapshot(snapshot, false),
    displayLines: [],
    cols: 0,
    updatedAt: Date.now(),
  };
  rebuildMemoryDisplayCache();
}

function ensureMemoryCache() {
  if (!state.memoryCache) {
    refreshMemoryCache();
  } else if (state.memoryCache.cols !== getCols()) {
    rebuildMemoryDisplayCache();
  }

  return state.memoryCache;
}

function hasGcMarkProfileControls() {
  return Ant.raw
    && typeof Ant.raw.gcMarkProfile === 'function'
    && typeof Ant.raw.gcMarkProfileEnable === 'function'
    && typeof Ant.raw.gcMarkProfileReset === 'function';
}

function toggleGcMarkProfile() {
  if (!hasGcMarkProfileControls()) {
    setMemoryStatus(`${c.red}GC mark profiler unavailable${c.reset}`);
    return false;
  }

  const current = Ant.raw.gcMarkProfile();
  const enabled = Ant.raw.gcMarkProfileEnable(!current.enabled);
  refreshMemoryCache();
  setMemoryStatus(enabled
    ? `${c.green}GC mark profiler enabled${c.reset}`
    : `${c.yellow}GC mark profiler disabled${c.reset}`);
  return true;
}

function resetGcMarkProfile() {
  if (!hasGcMarkProfileControls()) {
    setMemoryStatus(`${c.red}GC mark profiler unavailable${c.reset}`);
    return false;
  }

  Ant.raw.gcMarkProfileReset();
  refreshMemoryCache();
  setMemoryStatus(`${c.green}GC mark profiler reset${c.reset}`);
  return true;
}

function memoryStatusText() {
  if (!state.memoryStatus) return '';
  if (Date.now() - state.memoryStatusAt > 4000) {
    state.memoryStatus = '';
    return '';
  }
  return state.memoryStatus;
}

function setMemoryStatus(message) {
  state.memoryStatus = message;
  state.memoryStatusAt = Date.now();
}

function copyMemoryStats() {
  const text = ensureMemoryCache().plainLines.join('\n') + '\n';
  const commands = process.platform === 'win32'
    ? [{ command: 'clip', args: [] }]
    : [
        { command: 'pbcopy', args: [] },
        { command: 'wl-copy', args: [] },
        { command: 'xclip', args: ['-selection', 'clipboard'] },
        { command: 'xsel', args: ['--clipboard', '--input'] }
      ];

  for (const { command, args } of commands) {
    try {
      const result = spawnSync(command, args, { input: text });
      if (result.status === 0 || result.exitCode === 0) {
        setMemoryStatus(`${c.green}Copied stats to clipboard with ${command}${c.reset}`);
        return true;
      }
    } catch {}
  }

  setMemoryStatus(`${c.red}Clipboard copy failed${c.reset}`);
  return false;
}

function buildScreen() {
  const rows = getRows();
  const cols = getCols();
  const lines = [];
  let linesArePadded = false;

  if (state.mode === 'browse') {
    const rateColor = stats.rate >= 80 ? c.green : stats.rate >= 50 ? c.yellow : c.red;
    lines.push(
      `${c.bold}Total:${c.reset} ${stats.total}  ${c.green}Pass:${c.reset} ${stats.passed}  ${c.red}Fail:${c.reset} ${stats.failed}  ${rateColor}${stats.rate}%${c.reset}`
    );
    lines.push('');

    const filterLabel = state.filter === 'pass' ? 'PASS' : state.filter === 'fail' ? 'FAIL' : 'ALL';
    lines.push(`${c.dim}Filter: [${filterLabel}] · ${state.filtered.length}/${stats.total} tests${c.reset}`);

    if (state.searchMode) {
      lines.push(`${c.cyan}Search: ${state.searchQuery}█${c.reset}`);
    } else {
      lines.push(`${c.dim}↑↓ nav · p/f/a filter · s stats · m memory · / search · q quit${c.reset}`);
    }
    lines.push('');

    const headerLines = lines.length;
    const footerLines = 2;
    const listHeight = Math.max(1, rows - headerLines - footerLines);

    const total = state.filtered.length;
    const idx = state.index;
    const half = Math.floor(listHeight / 2);
    let start = Math.max(0, idx - half);
    let end = start + listHeight;
    if (end > total) {
      end = total;
      start = Math.max(0, end - listHeight);
    }

    for (let i = start; i < end; i++) {
      const [name, passed] = state.filtered[i];
      const icon = passed ? `${c.green}✓${c.reset}` : `${c.red}✗${c.reset}`;
      const displayName = truncate(name, cols - 4);

      if (i === idx) {
        lines.push(`${c.bgSelect}${c.bold} ${icon} ${pad(displayName, cols - 4)}${c.reset}`);
      } else {
        lines.push(` ${icon} ${displayName}`);
      }
    }

    while (lines.length < rows - footerLines) {
      lines.push('');
    }

    lines.push('');
    lines.push(`${c.dim}[${idx + 1}/${total}]${c.reset}  ${c.cyan}${fps.current} fps${c.reset}`);
  } else if (state.mode === 'stats') {
    const headerWidth = Math.min(cols, 68);
    lines.push(`${c.bold}${c.blue}${'═'.repeat(headerWidth)}${c.reset}`);
    lines.push(`${c.bold}  Results by Category${c.reset}`);
    lines.push(`${c.bold}${c.blue}${'═'.repeat(headerWidth)}${c.reset}`);
    lines.push('');

    const categories = {};
    entries.forEach(([name, passed]) => {
      const cat = name.split('/')[0];
      if (!categories[cat]) categories[cat] = { total: 0, passed: 0 };
      categories[cat].total++;
      if (passed) categories[cat].passed++;
    });

    const sorted = Object.entries(categories).sort((a, b) => b[1].passed - a[1].passed);
    const maxCatLen = Math.min(30, Math.max(...sorted.map(([k]) => k.length)));

    for (const [cat, s] of sorted) {
      const rate = ((s.passed / s.total) * 100).toFixed(0);
      const color = rate >= 80 ? c.green : rate >= 50 ? c.yellow : c.red;
      const barWidth = 20;
      const filled = Math.round((s.passed / s.total) * barWidth);
      const bar = `${c.green}${'█'.repeat(filled)}${c.dim}${'░'.repeat(barWidth - filled)}${c.reset}`;

      lines.push(
        `${truncate(cat, maxCatLen).padEnd(maxCatLen)} ${bar} ${s.passed.toString().padStart(5)}/${s.total.toString().padEnd(5)} ${color}${rate.padStart(3)}%${c.reset}`
      );
    }

    while (lines.length < rows - 2) {
      lines.push('');
    }

    lines.push('');
    lines.push(`${c.dim}s browse · q quit${c.reset}`);
  } else if (state.mode === 'memory') {
    const footerLines = 2;
    const memoryCache = ensureMemoryCache();
    const memoryLines = memoryCache.displayLines;
    const viewHeight = Math.max(1, rows - footerLines);
    const maxOffset = Math.max(0, memoryLines.length - viewHeight);

    if (state.memoryOffset > maxOffset) {
      state.memoryOffset = maxOffset;
    }

    const end = Math.min(memoryLines.length, state.memoryOffset + viewHeight);
    lines.push(...memoryLines.slice(state.memoryOffset, end));

    while (lines.length < rows - footerLines) {
      lines.push(' '.repeat(cols));
    }

    lines.push(' '.repeat(cols));
    const status = memoryStatusText();
    lines.push(pad(
      `${c.dim}↑↓ scroll · PgUp/PgDn · g/G top/bottom · r refresh · e profiler · R reset profiler · c copy · m browse · q quit${c.reset}  ${c.dim}[${state.memoryOffset + 1}-${end}/${memoryLines.length}]${c.reset}  ${status}${status ? '  ' : ''}${c.cyan}${fps.current} fps${c.reset}`,
      cols
    ));
    linesArePadded = true;
  }

  return linesArePadded ? lines.join('\n') : lines.map(l => pad(l, cols)).join('\n');
}

function render() {
  if (pendingRender) return;
  fps.update();
  const screen = buildScreen();
  const clear = state.fullClearNextRender ? term.clearScreen : '';
  state.fullClearNextRender = false;
  const ok = process.stdout.write(`${term.syncStart}${term.hideCursor}${term.home}${clear}${screen}${term.syncEnd}`);
  if (!ok) pendingRender = true;
}

function applyFilter(filter) {
  state.filter = filter;
  if (filter === 'pass') {
    state.filtered = entries.filter(([_, v]) => v);
  } else if (filter === 'fail') {
    state.filtered = entries.filter(([_, v]) => !v);
  } else {
    state.filtered = entries;
  }
  state.index = Math.min(state.index, Math.max(0, state.filtered.length - 1));
}

function applySearch(query) {
  if (!query) {
    state.filtered = entries;
  } else {
    const q = query.toLowerCase();
    state.filtered = entries.filter(([name]) => name.toLowerCase().includes(q));
  }
  state.index = 0;
}

function handleKey(key) {
  let needsRender = false;
  if (state.searchMode) {
    if (key === '\r' || key === '\n') {
      state.searchMode = false;
      applySearch(state.searchQuery);
      needsRender = true;
    } else if (key === '\x1b') {
      state.searchMode = false;
      state.searchQuery = '';
      needsRender = true;
    } else if (key === '\x7f' || key === '\b') {
      state.searchQuery = state.searchQuery.slice(0, -1);
      needsRender = true;
    } else if (key.length === 1 && key >= ' ') {
      state.searchQuery += key;
      needsRender = true;
    }
    return needsRender;
  }

  switch (key) {
    case 'q':
    case '\x03':
      cleanup();
      break;
    case '\x1b[A':
    case 'k':
      if (state.mode === 'memory') {
        state.memoryOffset = Math.max(0, state.memoryOffset - 1);
      } else {
        state.index = Math.max(0, state.index - 1);
      }
      needsRender = true;
      break;
    case '\x1b[B':
    case 'j':
      if (state.mode === 'memory') {
        state.memoryOffset++;
      } else {
        state.index = Math.min(state.filtered.length - 1, state.index + 1);
      }
      needsRender = true;
      break;
    case '\x1b[5~':
      if (state.mode === 'memory') {
        state.memoryOffset = Math.max(0, state.memoryOffset - Math.max(1, getRows() - 4));
      } else {
        state.index = Math.max(0, state.index - 10);
      }
      needsRender = true;
      break;
    case '\x1b[6~':
      if (state.mode === 'memory') {
        state.memoryOffset += Math.max(1, getRows() - 4);
      } else {
        state.index = Math.min(state.filtered.length - 1, state.index + 10);
      }
      needsRender = true;
      break;
    case 'g':
      if (state.mode === 'memory') {
        state.memoryOffset = 0;
      } else {
        state.index = 0;
      }
      needsRender = true;
      break;
    case 'G':
      if (state.mode === 'memory') {
        state.memoryOffset = Number.MAX_SAFE_INTEGER;
      } else {
        state.index = state.filtered.length - 1;
      }
      needsRender = true;
      break;
    case 'p':
      if (state.mode === 'browse') {
        applyFilter('pass');
        needsRender = true;
      }
      break;
    case 'f':
      if (state.mode === 'browse') {
        applyFilter('fail');
        needsRender = true;
      }
      break;
    case 'a':
      if (state.mode === 'browse') {
        applyFilter('');
        needsRender = true;
      }
      break;
    case 's':
      state.mode = state.mode === 'browse' ? 'stats' : 'browse';
      needsRender = true;
      break;
    case 'm':
      if (state.mode === 'memory') {
        state.mode = 'browse';
      } else {
        state.mode = 'memory';
        state.fullClearNextRender = true;
        state.memoryOffset = 0;
        refreshMemoryCache();
        setMemoryStatus(`${c.green}Refreshed memory stats${c.reset}`);
      }
      needsRender = true;
      break;
    case 'r':
      if (state.mode === 'memory') {
        refreshMemoryCache();
        setMemoryStatus(`${c.green}Refreshed memory stats${c.reset}`);
        needsRender = true;
      }
      break;
    case 'e':
      if (state.mode === 'memory') {
        needsRender = toggleGcMarkProfile() || needsRender;
      }
      break;
    case 'R':
      if (state.mode === 'memory') {
        needsRender = resetGcMarkProfile() || needsRender;
      }
      break;
    case 'c':
      if (state.mode === 'memory') {
        copyMemoryStats();
        needsRender = true;
      }
      break;
    case '/':
      if (state.mode === 'browse') {
        state.searchMode = true;
        state.searchQuery = '';
        needsRender = true;
      }
      break;
  }

  return needsRender;
}

function cleanup() {
  process.stdout.write(`${term.showCursor}${term.altScreenOff}`);
  process.stdin.setRawMode(false);
  process.stdin.pause();
  process.exit(0);
}

if (!process.stdin.isTTY || !process.stdout.isTTY) {
  console.log(`Total: ${stats.total}\nPassed: ${stats.passed}\nFailed: ${stats.failed}\nRate: ${stats.rate}%`);
  process.exit(0);
}

process.stdin.setRawMode(true);
process.stdin.resume();
process.stdout.write(`${term.altScreenOn}${term.hideCursor}`);

let inputBuf = '';
let pendingRender = false;
process.stdin.on('data', chunk => {
  const str = chunk.toString();
  let needsRender = false;

  for (const ch of str) {
    if (inputBuf.length > 0) {
      inputBuf += ch;
      if (inputBuf.length >= 3 && /[A-Za-z~]/.test(ch)) {
        needsRender = handleKey(inputBuf) || needsRender;
        inputBuf = '';
      }
    } else if (ch === '\x1b') {
      inputBuf = ch;
    } else {
      needsRender = handleKey(ch) || needsRender;
    }
  }

  if (needsRender) {
    render();
  }
});

process.stdout.on('resize', render);
process.stdout.on('drain', () => {
  if (!pendingRender) return;
  pendingRender = false;
  render();
});
process.on('SIGINT', cleanup);
process.on('SIGTERM', cleanup);

render();
