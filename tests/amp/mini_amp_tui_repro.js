function e4(char = " ", style = {}, width = 1) {
  return { char, style: { ...style }, width };
}

const bw = e4(" ", {});

function sameColor(a, b) {
  if (a === b) return true;
  if (a === void 0 || b === void 0) return false;
  if (a.type !== b.type) return false;
  switch (a.type) {
    case "index":
      return a.value === b.value;
    case "rgb":
      return a.value.r === b.value.r && a.value.g === b.value.g && a.value.b === b.value.b;
    default:
      return true;
  }
}

function sameStyle(a, b) {
  return (
    sameColor(a.fg, b.fg) &&
    sameColor(a.bg, b.bg) &&
    a.bold === b.bold &&
    a.italic === b.italic &&
    a.underline === b.underline &&
    a.dim === b.dim
  );
}

function sameCell(a, b) {
  return a.char === b.char && a.width === b.width && sameStyle(a.style, b.style);
}

class BufferGrid {
  constructor(width, height) {
    this.resize(width, height);
  }
  resize(width, height) {
    this.width = width;
    this.height = height;
    this.cells = Array(height)
      .fill(null)
      .map(() => Array(width).fill(null).map(() => bw));
  }
  getCell(x, y) {
    if (x < 0 || y < 0 || x >= this.width || y >= this.height) return null;
    return this.cells[y][x];
  }
  setCell(x, y, cell) {
    if (x < 0 || y < 0 || x >= this.width || y >= this.height) return;
    this.cells[y][x] = { ...cell, style: { ...cell.style } };
  }
  clear() {
    for (let y = 0; y < this.height; y++) {
      for (let x = 0; x < this.width; x++) this.cells[y][x] = bw;
    }
  }
}

class Screen {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    this.frontBuffer = new BufferGrid(width, height);
    this.backBuffer = new BufferGrid(width, height);
  }
  clear() {
    this.backBuffer.clear();
  }
  setCell(x, y, cell) {
    this.backBuffer.setCell(x, y, cell);
  }
  setText(x, y, text, style = {}) {
    for (let i = 0; i < text.length; i++) this.setCell(x + i, y, e4(text[i], style));
  }
  getDiff() {
    const out = [];
    for (let y = 0; y < this.height; y++) {
      for (let x = 0; x < this.width; x++) {
        const a = this.frontBuffer.getCell(x, y) ?? bw;
        const b = this.backBuffer.getCell(x, y) ?? bw;
        if (!sameCell(a, b)) out.push({ x, y, cell: b });
      }
    }
    return out;
  }
  present() {
    const old = this.frontBuffer;
    this.frontBuffer = this.backBuffer;
    this.backBuffer = old;
  }
}

class StringBuilder {
  constructor() {
    this.parts = [];
  }
  append(...xs) {
    this.parts.push(...xs);
  }
  toString() {
    return this.parts.join("");
  }
}

function moveTo(y, x) {
  return `\x1b[${y + 1};${x + 1}H`;
}

function ansiStyle(style) {
  const out = [];
  if (style.bold) out.push("1");
  if (style.dim) out.push("2");
  if (style.fg?.type === "index") out.push(String(30 + style.fg.value));
  if (style.bg?.type === "index") out.push(String(40 + style.bg.value));
  return out.length ? `\x1b[${out.join(";")}m` : "\x1b[0m";
}

function renderDiff(diff) {
  const sb = new StringBuilder();
  let lastStyle = null;
  for (const item of diff) {
    sb.append(moveTo(item.y, item.x));
    if (!lastStyle || !sameStyle(lastStyle, item.cell.style)) {
      sb.append(ansiStyle(item.cell.style));
      lastStyle = item.cell.style;
    }
    sb.append(item.cell.char);
  }
  sb.append("\x1b[0m");
  return sb.toString();
}

const width = process.stdout.columns || 80;
const height = process.stdout.rows || 24;
const screen = new Screen(width, height);

let tick = 0;
let text = "";
let menuOpen = false;
let running = true;

function paintFrame() {
  screen.clear();
  screen.setText(0, 0, "mini amp tui repro", { fg: { type: "index", value: 6 }, bold: true });
  screen.setText(0, 2, "type text, press m to toggle menu, q to quit", {
    fg: { type: "index", value: 7 },
  });
  screen.setText(0, 4, "textbox: [" + text + "]", {
    fg: { type: "index", value: 2 },
  });
  screen.setText(0, 6, menuOpen ? "menu: [open]" : "menu: [closed]", {
    fg: { type: "index", value: menuOpen ? 3 : 1 },
    bold: menuOpen,
  });
  const ballX = 8 + (tick % Math.max(1, Math.min(width - 10, 30)));
  screen.setText(0, 8, "ball:", { fg: { type: "index", value: 5 } });
  screen.setText(ballX, 8, "o", { fg: { type: "index", value: 4 }, bold: true });
  screen.setText(0, 10, "tick:" + tick, { fg: { type: "index", value: 7 } });

  const diff = screen.getDiff();
  const out = renderDiff(diff);
  process.stdout.write(out);
  screen.present();
}

function shutdown() {
  if (!running) return;
  running = false;
  clearInterval(timer);
  process.stdout.write("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
  if (process.stdin.isTTY && typeof process.stdin.setRawMode === "function") {
    process.stdin.setRawMode(false);
  }
  process.stdin.pause();
}

if (process.stdin.isTTY && typeof process.stdin.setRawMode === "function") {
  process.stdin.setRawMode(true);
}
process.stdin.resume();
process.stdin.on("data", (chunk) => {
  if (typeof chunk !== "string") chunk = String(chunk);
  for (const ch of chunk) {
    if (ch === "q" || ch === "\u0003") {
      shutdown();
      return;
    }
    if (ch === "m") {
      menuOpen = !menuOpen;
      paintFrame();
      continue;
    }
    if (ch === "\u007f") {
      text = text.slice(0, -1);
      paintFrame();
      continue;
    }
    if (ch >= " " && ch <= "~") {
      text += ch;
      paintFrame();
    }
  }
});

process.stdout.write("\x1b[2J\x1b[H\x1b[?25l");
paintFrame();
const timer = setInterval(() => {
  tick++;
  paintFrame();
}, 120);
