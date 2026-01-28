import * as readline from 'node:readline';

const ESC = '\x1b';
const CSI = `${ESC}[`;

export const codes = {
  hideCursor: `${CSI}?25l`,
  showCursor: `${CSI}?25h`,
  altScreenOn: `${CSI}?1049h`,
  altScreenOff: `${CSI}?1049l`,
  syncStart: `${CSI}?2026h`,
  syncEnd: `${CSI}?2026l`,
  home: `${CSI}H`,
  clear: `${CSI}2J`,
  clearLine: `${CSI}2K`,
  reset: `${CSI}0m`,
  bold: `${CSI}1m`,
  dim: `${CSI}2m`,
  italic: `${CSI}3m`,
  underline: `${CSI}4m`,
  blink: `${CSI}5m`,
  inverse: `${CSI}7m`,
  hidden: `${CSI}8m`,
  strikethrough: `${CSI}9m`,
  fg: n => `${CSI}38;5;${n}m`,
  bg: n => `${CSI}48;5;${n}m`,
  rgb: (r, g, b) => `${CSI}38;2;${r};${g};${b}m`,
  bgRgb: (r, g, b) => `${CSI}48;2;${r};${g};${b}m`,
  moveTo: (x, y) => `${CSI}${y + 1};${x + 1}H`,
  moveUp: n => `${CSI}${n}A`,
  moveDown: n => `${CSI}${n}B`,
  moveRight: n => `${CSI}${n}C`,
  moveLeft: n => `${CSI}${n}D`,
  saveCursor: `${ESC}7`,
  restoreCursor: `${ESC}8`,
  scrollUp: n => `${CSI}${n}S`,
  scrollDown: n => `${CSI}${n}T`
};

export const colors = {
  reset: codes.reset,
  bold: codes.bold,
  dim: codes.dim,
  italic: codes.italic,
  underline: codes.underline,
  inverse: codes.inverse,
  black: codes.fg(0),
  red: codes.fg(196),
  green: codes.fg(82),
  yellow: codes.fg(226),
  blue: codes.fg(39),
  magenta: codes.fg(201),
  cyan: codes.fg(51),
  white: codes.fg(15),
  gray: codes.fg(245),
  brightRed: codes.fg(9),
  brightGreen: codes.fg(10),
  brightYellow: codes.fg(11),
  brightBlue: codes.fg(12),
  brightMagenta: codes.fg(13),
  brightCyan: codes.fg(14),
  bgBlack: codes.bg(0),
  bgRed: codes.bg(196),
  bgGreen: codes.bg(82),
  bgYellow: codes.bg(226),
  bgBlue: codes.bg(24),
  bgMagenta: codes.bg(201),
  bgCyan: codes.bg(51),
  bgWhite: codes.bg(15),
  bgGray: codes.bg(238)
};

export const box = {
  light: { tl: '┌', tr: '┐', bl: '└', br: '┘', h: '─', v: '│', lT: '├', rT: '┤', tT: '┬', bT: '┴', cross: '┼' },
  heavy: { tl: '┏', tr: '┓', bl: '┗', br: '┛', h: '━', v: '┃', lT: '┣', rT: '┫', tT: '┳', bT: '┻', cross: '╋' },
  double: { tl: '╔', tr: '╗', bl: '╚', br: '╝', h: '═', v: '║', lT: '╠', rT: '╣', tT: '╦', bT: '╩', cross: '╬' },
  rounded: { tl: '╭', tr: '╮', bl: '╰', br: '╯', h: '─', v: '│', lT: '├', rT: '┤', tT: '┬', bT: '┴', cross: '┼' },
  ascii: { tl: '+', tr: '+', bl: '+', br: '+', h: '-', v: '|', lT: '+', rT: '+', tT: '+', bT: '+', cross: '+' }
};

export const keys = {
  UP: '\x1b[A',
  DOWN: '\x1b[B',
  RIGHT: '\x1b[C',
  LEFT: '\x1b[D',
  HOME: '\x1b[H',
  END: '\x1b[F',
  PAGE_UP: '\x1b[5~',
  PAGE_DOWN: '\x1b[6~',
  INSERT: '\x1b[2~',
  DELETE: '\x1b[3~',
  ENTER: '\r',
  TAB: '\t',
  ESCAPE: '\x1b',
  BACKSPACE: '\x7f',
  CTRL_C: '\x03',
  CTRL_D: '\x04',
  CTRL_Z: '\x1a',
  F1: '\x1bOP',
  F2: '\x1bOQ',
  F3: '\x1bOR',
  F4: '\x1bOS',
  F5: '\x1b[15~',
  F6: '\x1b[17~',
  F7: '\x1b[18~',
  F8: '\x1b[19~',
  F9: '\x1b[20~',
  F10: '\x1b[21~',
  F11: '\x1b[23~',
  F12: '\x1b[24~'
};

export function stripAnsi(str) {
  return str.replace(/\x1b\[[0-9;]*m/g, '');
}

export function visibleLength(str) {
  return stripAnsi(str).length;
}

export function pad(str, len, char = ' ') {
  const visible = visibleLength(str);
  const diff = len - visible;
  if (diff <= 0) return str;
  if (char === undefined || char === null) {
    console.error("pad() char undefined, str:", str, "len:", len);
    console.error(new Error().stack);
    process.exit(1);
  }
  return str + char.repeat(diff);
}

export function padStart(str, len, char = ' ') {
  const visible = visibleLength(str);
  return char.repeat(Math.max(0, len - visible)) + str;
}

export function padCenter(str, len, char = ' ') {
  const visible = visibleLength(str);
  const total = Math.max(0, len - visible);
  const left = Math.floor(total / 2);
  const right = total - left;
  return char.repeat(left) + str + char.repeat(right);
}

export function truncate(str, len, suffix = '…') {
  const stripped = stripAnsi(str);
  if (stripped.length <= len) return str;

  const suffixLen = visibleLength(suffix);
  const targetLen = len - suffixLen;
  if (targetLen <= 0) return suffix.slice(0, len);

  let visCount = 0;
  let result = '';
  let i = 0;

  while (i < str.length && visCount < targetLen) {
    if (str[i] === '\x1b') {
      const match = str.slice(i).match(/^\x1b\[[0-9;]*m/);
      if (match) {
        result += match[0];
        i += match[0].length;
        continue;
      }
    }
    result += str[i];
    visCount++;
    i++;
  }

  return result + codes.reset + suffix;
}

export function wrap(str, width) {
  const words = str.split(' ');
  const lines = [];
  let line = '';
  let lineLen = 0;

  for (const word of words) {
    const wordLen = visibleLength(word);
    const spaceNeeded = line ? 1 : 0;

    if (lineLen + wordLen + spaceNeeded > width) {
      if (line) lines.push(line);
      line = word;
      lineLen = wordLen;
    } else {
      line = line ? line + ' ' + word : word;
      lineLen += wordLen + spaceNeeded;
    }
  }
  if (line) lines.push(line);
  return lines;
}

export class Buffer {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    this.lines = Array(height).fill('').map(() => ' '.repeat(width));
    this.styles = Array(height).fill('').map(() => '');
  }

  clear() {
    for (let y = 0; y < this.height; y++) {
      this.lines[y] = ' '.repeat(this.width);
      this.styles[y] = '';
    }
  }

  resize(width, height) {
    const newLines = [];
    const newStyles = [];
    for (let y = 0; y < height; y++) {
      if (y < this.height) {
        newLines.push(pad(this.lines[y], width).slice(0, width));
        newStyles.push(this.styles[y]);
      } else {
        newLines.push(' '.repeat(width));
        newStyles.push('');
      }
    }
    this.width = width;
    this.height = height;
    this.lines = newLines;
    this.styles = newStyles;
  }

  write(x, y, text, style = '') {
    if (y < 0 || y >= this.height) return;
    const stripped = stripAnsi(text);
    const line = this.lines[y];
    const before = line.slice(0, Math.max(0, x));
    const after = line.slice(x + stripped.length);
    this.lines[y] = pad(before, x) + stripped + after;
    if (style) this.styles[y] = style;
  }

  writeStyled(x, y, text) {
    if (y < 0 || y >= this.height) return;
    const stripped = stripAnsi(text);
    const line = this.lines[y];
    const lineStripped = stripAnsi(line);

    const before = lineStripped.slice(0, Math.max(0, x));
    const after = lineStripped.slice(x + stripped.length);
    this.lines[y] = pad(before, x) + text + codes.reset + after;
  }

  fill(x, y, width, height, char = ' ', style = '') {
    for (let row = y; row < y + height && row < this.height; row++) {
      if (row < 0) continue;
      const fillStr = char.repeat(width);
      this.write(x, row, fillStr, style);
    }
  }

  box(x, y, width, height, style = box.light, title = '', titleStyle = '') {
    if (height < 2 || width < 2) return;
    if (!style || !style.h) {
      console.error("box() style undefined:", style);
      console.error(new Error().stack);
      process.exit(1);
    }

    const top = style.tl + style.h.repeat(width - 2) + style.tr;
    const bottom = style.bl + style.h.repeat(width - 2) + style.br;

    this.writeStyled(x, y, top);
    this.writeStyled(x, y + height - 1, bottom);

    for (let row = y + 1; row < y + height - 1 && row < this.height; row++) {
      if (row < 0) continue;
      this.writeStyled(x, row, style.v);
      this.writeStyled(x + width - 1, row, style.v);
    }

    if (title) {
      const titleStr = ` ${title} `;
      const titleX = x + Math.floor((width - visibleLength(titleStr)) / 2);
      this.writeStyled(titleX, y, titleStyle + titleStr + codes.reset);
    }
  }

  render() {
    return this.lines.join('\n');
  }

  clone() {
    const buf = new Buffer(this.width, this.height);
    buf.lines = [...this.lines];
    buf.styles = [...this.styles];
    return buf;
  }
}

export class Screen {
  constructor(options = {}) {
    this.stdin = options.stdin || process.stdin;
    this.stdout = options.stdout || process.stdout;
    this.fullscreen = options.fullscreen !== false;
    this.hideCursor = options.hideCursor !== false;
    this.rawMode = options.rawMode !== false;

    this._width = this.stdout.columns || 80;
    this._height = this.stdout.rows || 24;
    this._buffer = new Buffer(this._width, this._height);
    this._prevBuffer = null;
    this._running = false;
    this._keyHandlers = [];
    this._resizeHandlers = [];
    this._modalStack = [];

    this._onKeypress = this._onKeypress.bind(this);
    this._onResize = this._onResize.bind(this);
    this._cleanup = this._cleanup.bind(this);
  }

  get width() { return this._width; }
  get height() { return this._height; }
  get buffer() { return this._buffer; }

  start() {
    if (this._running) return;
    this._running = true;

    if (this.rawMode && this.stdin.isTTY) {
      this.stdin.setRawMode(true);
    }
    this.stdin.resume();
    readline.emitKeypressEvents(this.stdin);
    this.stdin.on('keypress', this._onKeypress);
    this.stdout.on('resize', this._onResize);
    process.on('SIGINT', this._cleanup);
    process.on('SIGTERM', this._cleanup);

    let init = '';
    if (this.fullscreen) init += codes.altScreenOn;
    if (this.hideCursor) init += codes.hideCursor;
    init += codes.home + codes.clear;
    this.stdout.write(init);
  }

  stop() {
    this._cleanup();
  }

  _cleanup() {
    if (!this._running) return;
    this._running = false;

    this.stdin.removeListener('keypress', this._onKeypress);
    this.stdout.removeListener('resize', this._onResize);
    process.removeListener('SIGINT', this._cleanup);
    process.removeListener('SIGTERM', this._cleanup);

    let cleanup = '';
    if (this.hideCursor) cleanup += codes.showCursor;
    if (this.fullscreen) cleanup += codes.altScreenOff;
    this.stdout.write(cleanup);

    if (this.rawMode && this.stdin.isTTY) {
      this.stdin.setRawMode(false);
    }
    this.stdin.pause();
  }

  _onResize() {
    this._width = this.stdout.columns || 80;
    this._height = this.stdout.rows || 24;
    this._buffer.resize(this._width, this._height);
    this._prevBuffer = null;
    for (const handler of this._resizeHandlers) {
      handler(this._width, this._height);
    }
  }

  _onKeypress(str, key) {
    const sequence = key && key.sequence ? key.sequence : str;
    if (!sequence) return;
    this._emitKey(sequence);
  }

  _emitKey(key) {
    if (this._modalStack.length > 0) {
      const modal = this._modalStack[this._modalStack.length - 1];
      if (modal.onKey) {
        const result = modal.onKey(key, modal);
        if (result === false) return;
      }
    }

    for (const handler of this._keyHandlers) {
      handler(key);
    }
  }

  onKey(handler) {
    this._keyHandlers.push(handler);
    return () => {
      const idx = this._keyHandlers.indexOf(handler);
      if (idx >= 0) this._keyHandlers.splice(idx, 1);
    };
  }

  onResize(handler) {
    this._resizeHandlers.push(handler);
    return () => {
      const idx = this._resizeHandlers.indexOf(handler);
      if (idx >= 0) this._resizeHandlers.splice(idx, 1);
    };
  }

  clear() {
    this._buffer.clear();
  }

  write(x, y, text, style = '') {
    this._buffer.writeStyled(x, y, style + text);
  }

  fill(x, y, width, height, char = ' ', style = '') {
    for (let row = y; row < y + height && row < this._height; row++) {
      if (row < 0) continue;
      this.write(x, row, style + char.repeat(width));
    }
  }

  box(x, y, width, height, style = box.light, title = '', titleStyle = '') {
    this._buffer.box(x, y, width, height, style, title, titleStyle);
  }

  pushModal(options) {
    const savedBuffer = this._buffer.clone();
    const modal = {
      id: options.id || Date.now().toString(),
      x: options.x,
      y: options.y,
      width: options.width,
      height: options.height,
      savedBuffer,
      onKey: options.onKey,
      onRender: options.onRender
    };
    this._modalStack.push(modal);
    return modal;
  }

  popModal(id) {
    let idx = -1;
    if (id) {
      idx = this._modalStack.findIndex(m => m.id === id);
    } else {
      idx = this._modalStack.length - 1;
    }

    if (idx >= 0) {
      const modal = this._modalStack[idx];
      this._modalStack.splice(idx, 1);
      this._buffer = modal.savedBuffer;
      return true;
    }
    return false;
  }

  hasModal(id) {
    if (id) return this._modalStack.some(m => m.id === id);
    return this._modalStack.length > 0;
  }

  getModal(id) {
    if (id) return this._modalStack.find(m => m.id === id);
    return this._modalStack[this._modalStack.length - 1];
  }

  renderModal(modal, renderFn) {
    const { x, y, width, height, savedBuffer } = modal;

    for (let row = 0; row < this._height; row++) {
      this._buffer.lines[row] = savedBuffer.lines[row];
    }

    const modalBuffer = new Buffer(width, height);
    renderFn(modalBuffer, width, height);

    for (let row = 0; row < height && y + row < this._height; row++) {
      if (y + row < 0) continue;
      const modalLine = modalBuffer.lines[row];
      const bgLine = this._buffer.lines[y + row];
      const bgStripped = stripAnsi(bgLine);

      const before = bgStripped.slice(0, Math.max(0, x));
      const after = bgStripped.slice(x + width);

      this._buffer.lines[y + row] = pad(before, x) + modalLine + after;
    }
  }

  render() {
    for (const modal of this._modalStack) {
      if (modal.onRender) {
        this.renderModal(modal, modal.onRender);
      }
    }

    const output = this._buffer.render();
    this.stdout.write(
      codes.syncStart +
      codes.home +
      output +
      codes.syncEnd
    );
    this._prevBuffer = this._buffer.clone();
  }

  exit(code = 0) {
    this._cleanup();
    process.exit(code);
  }
}

export class List {
  constructor(options = {}) {
    this.items = options.items || [];
    this.index = options.index || 0;
    this.x = options.x || 0;
    this.y = options.y || 0;
    this.width = options.width || 40;
    this.height = options.height || 10;
    this.selectedStyle = options.selectedStyle || colors.bgBlue + colors.bold;
    this.normalStyle = options.normalStyle || '';
    this.renderItem = options.renderItem || (item => String(item));
    this.scrollOffset = 0;
  }

  setItems(items) {
    this.items = items;
    this.index = Math.min(this.index, Math.max(0, items.length - 1));
    this._updateScroll();
  }

  select(index) {
    this.index = Math.max(0, Math.min(this.items.length - 1, index));
    this._updateScroll();
  }

  selectNext() {
    this.select(this.index + 1);
  }

  selectPrev() {
    this.select(this.index - 1);
  }

  pageDown(amount = 10) {
    this.select(this.index + amount);
  }

  pageUp(amount = 10) {
    this.select(this.index - amount);
  }

  selectFirst() {
    this.select(0);
  }

  selectLast() {
    this.select(this.items.length - 1);
  }

  getSelected() {
    return this.items[this.index];
  }

  _updateScroll() {
    const half = Math.floor(this.height / 2);
    let start = this.index - half;
    if (start < 0) start = 0;
    if (start + this.height > this.items.length) {
      start = Math.max(0, this.items.length - this.height);
    }
    this.scrollOffset = start;
  }

  handleKey(key) {
    switch (key) {
      case keys.UP:
      case 'k':
        this.selectPrev();
        return true;
      case keys.DOWN:
      case 'j':
        this.selectNext();
        return true;
      case keys.PAGE_UP:
        this.pageUp();
        return true;
      case keys.PAGE_DOWN:
        this.pageDown();
        return true;
      case 'g':
        this.selectFirst();
        return true;
      case 'G':
        this.selectLast();
        return true;
    }
    return false;
  }

  render(screen) {
    const end = Math.min(this.scrollOffset + this.height, this.items.length);

    for (let i = this.scrollOffset; i < end; i++) {
      const row = this.y + (i - this.scrollOffset);
      const item = this.items[i];
      const text = truncate(this.renderItem(item, i), this.width);
      const isSelected = i === this.index;
      const style = isSelected ? this.selectedStyle : this.normalStyle;

      screen.write(this.x, row, pad(style + text + codes.reset, this.width));
    }

    for (let i = end - this.scrollOffset; i < this.height; i++) {
      screen.write(this.x, this.y + i, ' '.repeat(this.width));
    }
  }
}

export class ProgressBar {
  constructor(options = {}) {
    this.value = options.value || 0;
    this.max = options.max || 100;
    this.width = options.width || 20;
    this.filled = options.filled || '█';
    this.empty = options.empty || '░';
    this.filledStyle = options.filledStyle || colors.green;
    this.emptyStyle = options.emptyStyle || colors.dim;
    this.showPercent = options.showPercent !== false;
  }

  setValue(value) {
    this.value = Math.max(0, Math.min(this.max, value));
  }

  render() {
    const ratio = this.value / this.max;
    const filledCount = Math.round(ratio * this.width);
    const emptyCount = this.width - filledCount;

    let bar = this.filledStyle + this.filled.repeat(filledCount) + codes.reset;
    bar += this.emptyStyle + this.empty.repeat(emptyCount) + codes.reset;

    if (this.showPercent) {
      const percent = (ratio * 100).toFixed(0);
      bar += ` ${percent}%`;
    }

    return bar;
  }
}

export class Input {
  constructor(options = {}) {
    this.value = options.value || '';
    this.placeholder = options.placeholder || '';
    this.width = options.width || 20;
    this.cursorPos = this.value.length;
    this.style = options.style || '';
    this.cursorStyle = options.cursorStyle || colors.inverse;
  }

  setValue(value) {
    this.value = value;
    this.cursorPos = value.length;
  }

  clear() {
    this.value = '';
    this.cursorPos = 0;
  }

  handleKey(key) {
    if (key === keys.BACKSPACE || key === '\b') {
      if (this.cursorPos > 0) {
        this.value = this.value.slice(0, this.cursorPos - 1) + this.value.slice(this.cursorPos);
        this.cursorPos--;
      }
      return true;
    } else if (key === keys.DELETE) {
      if (this.cursorPos < this.value.length) {
        this.value = this.value.slice(0, this.cursorPos) + this.value.slice(this.cursorPos + 1);
      }
      return true;
    } else if (key === keys.LEFT) {
      this.cursorPos = Math.max(0, this.cursorPos - 1);
      return true;
    } else if (key === keys.RIGHT) {
      this.cursorPos = Math.min(this.value.length, this.cursorPos + 1);
      return true;
    } else if (key === keys.HOME) {
      this.cursorPos = 0;
      return true;
    } else if (key === keys.END) {
      this.cursorPos = this.value.length;
      return true;
    } else if (key.length === 1 && key >= ' ' && key <= '~') {
      this.value = this.value.slice(0, this.cursorPos) + key + this.value.slice(this.cursorPos);
      this.cursorPos++;
      return true;
    }
    return false;
  }

  render() {
    let display = this.value || this.placeholder;
    const isPlaceholder = !this.value && this.placeholder;

    if (isPlaceholder) {
      display = colors.dim + display + codes.reset;
    }

    if (this.value.length > 0) {
      const before = this.value.slice(0, this.cursorPos);
      const cursor = this.value[this.cursorPos] || ' ';
      const after = this.value.slice(this.cursorPos + 1);
      display = this.style + before + this.cursorStyle + cursor + codes.reset + this.style + after + codes.reset;
    } else {
      display = this.style + this.cursorStyle + ' ' + codes.reset;
    }

    return truncate(display, this.width);
  }
}

export class Table {
  constructor(options = {}) {
    this.columns = options.columns || [];
    this.rows = options.rows || [];
    this.x = options.x || 0;
    this.y = options.y || 0;
    this.width = options.width;
    this.headerStyle = options.headerStyle || colors.bold;
    this.rowStyle = options.rowStyle || '';
    this.altRowStyle = options.altRowStyle || colors.dim;
    this.borderStyle = options.borderStyle || box.light;
    this.showBorder = options.showBorder !== false;
  }

  _calcColumnWidths() {
    const widths = this.columns.map(col => visibleLength(col.header || col.key));

    for (const row of this.rows) {
      for (let i = 0; i < this.columns.length; i++) {
        const col = this.columns[i];
        const value = String(row[col.key] ?? '');
        widths[i] = Math.max(widths[i], visibleLength(value));
      }
    }

    if (this.width) {
      const totalWidth = widths.reduce((a, b) => a + b, 0);
      const available = this.width - (this.showBorder ? this.columns.length + 1 : 0);
      if (totalWidth > available) {
        const scale = available / totalWidth;
        for (let i = 0; i < widths.length; i++) {
          widths[i] = Math.floor(widths[i] * scale);
        }
      }
    }

    return widths;
  }

  render(screen) {
    const widths = this._calcColumnWidths();
    const bs = this.borderStyle;
    let row = this.y;

    if (this.showBorder) {
      let top = bs.tl;
      for (let i = 0; i < widths.length; i++) {
        top += bs.h.repeat(widths[i] + 2);
        top += i < widths.length - 1 ? bs.tT : bs.tr;
      }
      screen.write(this.x, row++, top);
    }

    let header = this.showBorder ? bs.v : '';
    for (let i = 0; i < this.columns.length; i++) {
      const col = this.columns[i];
      const text = pad(col.header || col.key, widths[i]);
      header += ' ' + this.headerStyle + text + codes.reset + ' ';
      if (this.showBorder) header += bs.v;
    }
    screen.write(this.x, row++, header);

    if (this.showBorder) {
      let sep = bs.lT;
      for (let i = 0; i < widths.length; i++) {
        sep += bs.h.repeat(widths[i] + 2);
        sep += i < widths.length - 1 ? bs.cross : bs.rT;
      }
      screen.write(this.x, row++, sep);
    }

    for (let r = 0; r < this.rows.length; r++) {
      const data = this.rows[r];
      const style = r % 2 === 0 ? this.rowStyle : this.altRowStyle;
      let line = this.showBorder ? bs.v : '';
      for (let i = 0; i < this.columns.length; i++) {
        const col = this.columns[i];
        const value = String(data[col.key] ?? '');
        const text = pad(truncate(value, widths[i]), widths[i]);
        line += ' ' + style + text + codes.reset + ' ';
        if (this.showBorder) line += bs.v;
      }
      screen.write(this.x, row++, line);
    }

    if (this.showBorder) {
      let bottom = bs.bl;
      for (let i = 0; i < widths.length; i++) {
        bottom += bs.h.repeat(widths[i] + 2);
        bottom += i < widths.length - 1 ? bs.bT : bs.br;
      }
      screen.write(this.x, row++, bottom);
    }

    return row - this.y;
  }
}

export function modal(screen, options) {
  const width = options.width || 40;
  const height = options.height || 10;
  const x = options.x ?? Math.floor((screen.width - width) / 2);
  const y = options.y ?? Math.floor((screen.height - height) / 2);
  const title = options.title || '';
  const titleStyle = options.titleStyle || colors.bold;
  const borderStyle = options.borderStyle || box.rounded;
  const bgStyle = options.bgStyle || '';

  return screen.pushModal({
    id: options.id,
    x,
    y,
    width,
    height,
    onKey: options.onKey,
    onRender: (buf, w, h) => {
      buf.fill(0, 0, w, h, ' ');
      buf.box(0, 0, w, h, borderStyle, title, titleStyle);

      if (options.render) {
        options.render(buf, w - 2, h - 2, 1, 1);
      }
    }
  });
}

export function confirm(screen, options) {
  return new Promise(resolve => {
    const message = options.message || 'Are you sure?';
    const width = Math.max(visibleLength(message) + 4, 30);
    const height = 7;

    modal(screen, {
      id: 'confirm',
      width,
      height,
      title: options.title || 'Confirm',
      titleStyle: colors.bold + colors.yellow,
      onKey: key => {
        if (key === 'y' || key === 'Y' || key === keys.ENTER) {
          screen.popModal('confirm');
          screen.render();
          resolve(true);
        } else if (key === 'n' || key === 'N' || key === keys.ESCAPE || key === keys.CTRL_C) {
          screen.popModal('confirm');
          screen.render();
          resolve(false);
        }
        return false;
      },
      render: (buf, w, h, ox, oy) => {
        buf.writeStyled(ox, oy + 1, padCenter(message, w));
        buf.writeStyled(ox, oy + 3, padCenter(`${colors.green}[Y]es${codes.reset}  ${colors.red}[N]o${codes.reset}`, w + 20));
      }
    });
    screen.render();
  });
}

export function alert(screen, options) {
  return new Promise(resolve => {
    const message = options.message || '';
    const lines = wrap(message, 50);
    const width = Math.max(...lines.map(visibleLength), 20) + 4;
    const height = lines.length + 5;

    modal(screen, {
      id: 'alert',
      width,
      height,
      title: options.title || 'Alert',
      titleStyle: colors.bold + colors.cyan,
      onKey: key => {
        if (key === keys.ENTER || key === keys.ESCAPE || key === ' ') {
          screen.popModal('alert');
          screen.render();
          resolve();
        }
        return false;
      },
      render: (buf, w, h, ox, oy) => {
        for (let i = 0; i < lines.length; i++) {
          buf.writeStyled(ox, oy + i + 1, padCenter(lines[i], w));
        }
        buf.writeStyled(ox, oy + lines.length + 2, padCenter(`${colors.dim}[Enter] OK${codes.reset}`, w + 10));
      }
    });
    screen.render();
  });
}

export default {
  codes,
  colors,
  box,
  keys,
  stripAnsi,
  visibleLength,
  pad,
  padStart,
  padCenter,
  truncate,
  wrap,
  Buffer,
  Screen,
  List,
  ProgressBar,
  Input,
  Table,
  modal,
  confirm,
  alert
};
