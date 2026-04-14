function e4(char = " ", style = {}, width = 1, hyperlink) {
  return { char, style: { ...style }, width, hyperlink };
}

const bw = e4(" ", {});

function sameColor(a, b) {
  if (a === b) return true;
  if (a === void 0 || b === void 0) return false;
  if (a.type !== b.type) return false;
  if (a.alpha !== b.alpha) return false;
  if (a.type === "index") return a.value === b.value;
  if (a.type === "rgb")
    return a.value.r === b.value.r && a.value.g === b.value.g && a.value.b === b.value.b;
  return true;
}

function sameStyle(a, b) {
  return (
    sameColor(a.fg, b.fg) &&
    sameColor(a.bg, b.bg) &&
    a.bold === b.bold &&
    a.italic === b.italic &&
    a.underline === b.underline &&
    a.strikethrough === b.strikethrough &&
    a.reverse === b.reverse &&
    a.dim === b.dim
  );
}

function sameCell(a, b) {
  return (
    a.char === b.char &&
    a.width === b.width &&
    sameStyle(a.style, b.style) &&
    a.hyperlink === b.hyperlink
  );
}

class BufferGrid {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    this.resize(width, height);
  }
  resize(width, height) {
    this.width = width;
    this.height = height;
    this.cells = Array(height).fill(null).map(() => Array(width).fill(null).map(() => bw));
  }
  setCell(x, y, cell) {
    this.cells[y][x] = { ...cell, style: { ...cell.style } };
  }
  getCell(x, y) {
    return this.cells[y][x];
  }
  clear() {
    for (let y = 0; y < this.height; y++) {
      for (let x = 0; x < this.width; x++) this.cells[y][x] = bw;
    }
  }
  rowString(y) {
    let out = "";
    for (let x = 0; x < this.width; x++) out += this.cells[y][x].char;
    return out;
  }
}

class Screen {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    this.frontBuffer = new BufferGrid(width, height);
    this.backBuffer = new BufferGrid(width, height);
  }
  getBuffer() {
    return this.backBuffer;
  }
  clear() {
    this.backBuffer.clear();
  }
  setCell(x, y, cell) {
    this.backBuffer.setCell(x, y, cell);
  }
  getDiff() {
    const diff = [];
    for (let y = 0; y < this.height; y++) {
      for (let x = 0; x < this.width; x++) {
        const a = this.frontBuffer.getCell(x, y);
        const b = this.backBuffer.getCell(x, y);
        if (!sameCell(a, b)) diff.push({ x, y, cell: b });
      }
    }
    return diff;
  }
  present() {
    const old = this.frontBuffer;
    this.frontBuffer = this.backBuffer;
    this.backBuffer = old;
  }
}

function drawText(screen, x, y, text) {
  for (let i = 0; i < text.length; i++) {
    screen.setCell(x + i, y, e4(text[i], { fg: { type: "index", value: 7 } }));
  }
}

const screen = new Screen(20, 4);

for (let frame = 0; frame < 5; frame++) {
  screen.clear();
  drawText(screen, 0, 0, "frame:" + frame);
  drawText(screen, 0, 1, "ball:" + " ".repeat(frame) + "o");
  const diff = screen.getDiff();
  console.log(JSON.stringify({
    frame,
    diffCount: diff.length,
    row0: screen.backBuffer.rowString(0),
    row1: screen.backBuffer.rowString(1),
  }));
  screen.present();
}
