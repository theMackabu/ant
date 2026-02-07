import fs from 'fs';
import { join } from 'path';

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

function buildScreen() {
  const rows = getRows();
  const cols = getCols();
  const lines = [];

  if (state.mode === 'browse') {
    const headerWidth = Math.min(cols, 68);
    lines.push(`${c.bold}${c.blue}${'═'.repeat(headerWidth)}${c.reset}`);
    lines.push(`${c.bold}${c.blue}  Test262 Results Visualizer${c.reset}`);
    lines.push(`${c.bold}${c.blue}${'═'.repeat(headerWidth)}${c.reset}`);
    lines.push('');

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
    const mem = Ant.stats();
    const headerWidth = Math.min(cols, 68);
    lines.push(`${c.bold}${c.blue}${'═'.repeat(headerWidth)}${c.reset}`);
    lines.push(`${c.bold}  Memory Usage${c.reset}`);
    lines.push(`${c.bold}${c.blue}${'═'.repeat(headerWidth)}${c.reset}`);
    lines.push('');

    lines.push(`${c.cyan}Arena${c.reset}`);
    lines.push(`  Used:     ${c.bold}${fmt(mem.arenaUsed)}${c.reset}`);
    lines.push(`  Size:     ${c.bold}${fmt(mem.arenaSize)}${c.reset}`);
    lines.push('');

    if (mem.external) {
      lines.push(`${c.cyan}External${c.reset}`);
      lines.push(`  Buffers:  ${c.bold}${fmt(mem.external.buffers)}${c.reset}`);
      lines.push(`  Code:     ${c.bold}${fmt(mem.external.code)}${c.reset}`);
      lines.push(`  Collections: ${c.bold}${fmt(mem.external.collections)}${c.reset}`);
      lines.push(`  Total:    ${c.bold}${fmt(mem.external.total)}${c.reset}`);
      lines.push('');
    }

    lines.push(`${c.cyan}Process${c.reset}`);
    lines.push(`  RSS:      ${c.bold}${fmt(mem.rss)}${c.reset}`);
    if (mem.virtualSize) {
      lines.push(`  Virtual:  ${c.bold}${fmt(mem.virtualSize)}${c.reset}`);
    }
    lines.push('');

    lines.push(`${c.cyan}C Stack${c.reset}`);
    lines.push(`  Max:      ${c.bold}${fmt(mem.cstack)}${c.reset}`);
    lines.push('');

    lines.push(`${c.dim}Press 'g' to run GC${c.reset}`);

    while (lines.length < rows - 2) {
      lines.push('');
    }

    lines.push('');
    lines.push(`${c.dim}m browse · g gc · q quit${c.reset}  ${c.cyan}${fps.current} fps${c.reset}`);
  }

  return lines.map(l => pad(l, cols)).join('\n');
}

function render() {
  if (pendingRender) return;
  fps.update();
  const screen = buildScreen();
  const ok = process.stdout.write(`${term.syncStart}${term.hideCursor}${term.home}${screen}${term.syncEnd}`);
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
      state.index = Math.max(0, state.index - 1);
      needsRender = true;
      break;
    case '\x1b[B':
    case 'j':
      state.index = Math.min(state.filtered.length - 1, state.index + 1);
      needsRender = true;
      break;
    case '\x1b[5~':
      state.index = Math.max(0, state.index - 10);
      needsRender = true;
      break;
    case '\x1b[6~':
      state.index = Math.min(state.filtered.length - 1, state.index + 10);
      needsRender = true;
      break;
    case 'g':
      if (state.mode === 'memory') {
        Ant.gc();
      } else {
        state.index = 0;
      }
      needsRender = true;
      break;
    case 'G':
      state.index = state.filtered.length - 1;
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
      state.mode = state.mode === 'memory' ? 'browse' : 'memory';
      needsRender = true;
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
