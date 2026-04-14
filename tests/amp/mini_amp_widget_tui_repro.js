function e4(char = " ", style = {}, width = 1) {
  return { char, style: { ...style }, width };
}

const bw = e4(" ", {});

function sameColor(a, b) {
  if (a === b) return true;
  if (a === void 0 || b === void 0) return false;
  if (a.type !== b.type) return false;
  if (a.type === "index") return a.value === b.value;
  return true;
}

function sameStyle(a, b) {
  return sameColor(a.fg, b.fg) && sameColor(a.bg, b.bg) && a.bold === b.bold;
}

function sameCell(a, b) {
  return a.char === b.char && a.width === b.width && sameStyle(a.style, b.style);
}

class BufferGrid {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    this.cells = Array(height).fill(null).map(() => Array(width).fill(null).map(() => bw));
  }
  clear() {
    for (let y = 0; y < this.height; y++) {
      for (let x = 0; x < this.width; x++) this.cells[y][x] = bw;
    }
  }
  setCell(x, y, cell) {
    if (x < 0 || y < 0 || x >= this.width || y >= this.height) return;
    this.cells[y][x] = { ...cell, style: { ...cell.style } };
  }
  getCell(x, y) {
    return this.cells[y][x];
  }
}

class Screen {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    this.front = new BufferGrid(width, height);
    this.back = new BufferGrid(width, height);
  }
  clear() {
    this.back.clear();
  }
  setText(x, y, text, style = {}) {
    for (let i = 0; i < text.length; i++) this.back.setCell(x + i, y, e4(text[i], style));
  }
  getDiff() {
    const out = [];
    for (let y = 0; y < this.height; y++) {
      for (let x = 0; x < this.width; x++) {
        const a = this.front.getCell(x, y);
        const b = this.back.getCell(x, y);
        if (!sameCell(a, b)) out.push({ x, y, cell: b });
      }
    }
    return out;
  }
  present() {
    const old = this.front;
    this.front = this.back;
    this.back = old;
  }
}

function moveTo(y, x) {
  return `\x1b[${y + 1};${x + 1}H`;
}

function styleCode(style) {
  const out = [];
  if (style.bold) out.push("1");
  if (style.fg?.type === "index") out.push(String(30 + style.fg.value));
  return out.length ? `\x1b[${out.join(";")}m` : "\x1b[0m";
}

function renderDiff(diff) {
  const parts = [];
  let lastStyle = null;
  for (const item of diff) {
    parts.push(moveTo(item.y, item.x));
    if (!lastStyle || !sameStyle(lastStyle, item.cell.style)) {
      parts.push(styleCode(item.cell.style));
      lastStyle = item.cell.style;
    }
    parts.push(item.cell.char);
  }
  parts.push("\x1b[0m");
  return parts.join("");
}

let BUILD_OWNER = null;
let PIPELINE = null;
let ROOT_ELEMENT = null;
const SCREEN = new Screen(process.stdout.columns || 80, process.stdout.rows || 24);

class Widget {
  canUpdate(other) {
    return this.constructor === other.constructor;
  }
}

class Element {
  constructor(widget) {
    this.widget = widget;
    this.parent = void 0;
    this._dirty = false;
    this._mounted = false;
  }
  markMounted() {
    this._mounted = true;
  }
  markNeedsRebuild() {
    if (!this._mounted) return;
    this._dirty = true;
    BUILD_OWNER.scheduleBuildFor(this);
  }
  markNeedsBuild() {
    this.markNeedsRebuild();
  }
}

class BuildOwner {
  constructor() {
    this.dirty = new Set();
  }
  scheduleBuildFor(el) {
    this.dirty.add(el);
  }
  buildScopes() {
    const batch = Array.from(this.dirty);
    this.dirty.clear();
    for (const el of batch) {
      if (el._dirty) {
        el.performRebuild();
        el._dirty = false;
      }
    }
  }
}

class BuildContext {
  constructor(element, widget) {
    this.element = element;
    this.widget = widget;
  }
}

class State {
  _mount(widget, context) {
    this.widget = widget;
    this.context = context;
    this._mounted = true;
    this.initState?.();
  }
  _update(widget) {
    const oldWidget = this.widget;
    this.widget = widget;
    this.didUpdateWidget?.(oldWidget);
  }
  setState(fn) {
    if (fn) fn();
    this.context.element.markNeedsBuild();
  }
}

class RenderObject {
  constructor() {
    this._attached = false;
    this._needsLayout = true;
    this._needsPaint = true;
  }
  attach() {
    this._attached = true;
  }
  markNeedsLayout() {
    if (!this._attached) return;
    this._needsLayout = true;
  }
  markNeedsPaint() {
    if (!this._attached) return;
    this._needsPaint = true;
  }
}

class LabelRenderObject extends RenderObject {
  constructor(line, text, style) {
    super();
    this.line = line;
    this.text = text;
    this.style = style;
  }
  update(line, text, style) {
    this.line = line;
    this.text = text;
    this.style = style;
    this.markNeedsLayout();
    this.markNeedsPaint();
  }
  paint(screen) {
    screen.setText(0, this.line, this.text, this.style);
    this._needsPaint = false;
    this._needsLayout = false;
  }
}

class RenderObjectWidget extends Widget {
  createElement() {
    return new RenderObjectElement(this);
  }
}

class RenderObjectElement extends Element {
  mount() {
    this.renderObject = this.widget.createRenderObject();
    this.renderObject.attach();
    this.markMounted();
  }
  update(widget) {
    this.widget = widget;
    this.widget.updateRenderObject(this.renderObject);
  }
  performRebuild() {}
}

class StatefulWidget extends Widget {
  createElement() {
    return new StatefulElement(this);
  }
}

class StatefulElement extends Element {
  mount() {
    this.context = new BuildContext(this, this.widget);
    this.state = this.widget.createState();
    this.state._mount(this.widget, this.context);
    this.rebuild();
    this.markMounted();
  }
  rebuild() {
    const widgets = this.state.build(this.context);
    if (!this.children) {
      this.children = widgets.map((w) => {
        const el = w.createElement();
        el.parent = this;
        el.mount();
        return el;
      });
      return;
    }
    for (let i = 0; i < widgets.length; i++) {
      const next = widgets[i];
      const child = this.children[i];
      if (child.widget.canUpdate(next)) child.update(next);
    }
  }
  performRebuild() {
    this.rebuild();
  }
}

class Label extends RenderObjectWidget {
  constructor(line, text, style) {
    super();
    this.line = line;
    this.text = text;
    this.style = style;
  }
  createRenderObject() {
    return new LabelRenderObject(this.line, this.text, this.style);
  }
  updateRenderObject(ro) {
    ro.update(this.line, this.text, this.style);
  }
}

class App extends StatefulWidget {
  createState() {
    return new AppState();
  }
}

class AppState extends State {
  initState() {
    this.tick = 0;
    this.text = "";
    this.menuOpen = false;
  }
  build() {
    return [
      new Label(0, "widget-tree repro", { fg: { type: "index", value: 6 }, bold: true }),
      new Label(2, "textbox: [" + this.text + "]", { fg: { type: "index", value: 2 } }),
      new Label(4, this.menuOpen ? "menu: [open]" : "menu: [closed]", {
        fg: { type: "index", value: this.menuOpen ? 3 : 1 },
        bold: this.menuOpen,
      }),
      new Label(6, "ball: " + " ".repeat(this.tick % 20) + "o", {
        fg: { type: "index", value: 4 },
      }),
      new Label(8, "tick:" + this.tick, { fg: { type: "index", value: 7 } }),
    ];
  }
}

class Pipeline {
  paint() {
    SCREEN.clear();
    for (const child of ROOT_ELEMENT.children) {
      if (child.renderObject._needsLayout || child.renderObject._needsPaint) {
        child.renderObject.paint(SCREEN);
      } else {
        child.renderObject.paint(SCREEN);
      }
    }
    process.stdout.write(renderDiff(SCREEN.getDiff()));
    SCREEN.present();
  }
}

BUILD_OWNER = new BuildOwner();
PIPELINE = new Pipeline();
ROOT_ELEMENT = new App().createElement();
ROOT_ELEMENT.mount();

function frame() {
  BUILD_OWNER.buildScopes();
  PIPELINE.paint();
}

if (process.stdin.isTTY && typeof process.stdin.setRawMode === "function") {
  process.stdin.setRawMode(true);
}
process.stdin.resume();
process.stdin.on("data", (chunk) => {
  if (typeof chunk !== "string") chunk = String(chunk);
  for (const ch of chunk) {
    if (ch === "q" || ch === "\u0003") {
      clearInterval(timer);
      process.stdout.write("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
      process.exit(0);
    }
    if (ch === "m") {
      ROOT_ELEMENT.state.setState(() => {
        ROOT_ELEMENT.state.menuOpen = !ROOT_ELEMENT.state.menuOpen;
      });
      frame();
      continue;
    }
    if (ch === "\u007f") {
      ROOT_ELEMENT.state.setState(() => {
        ROOT_ELEMENT.state.text = ROOT_ELEMENT.state.text.slice(0, -1);
      });
      frame();
      continue;
    }
    if (ch >= " " && ch <= "~") {
      ROOT_ELEMENT.state.setState(() => {
        ROOT_ELEMENT.state.text += ch;
      });
      frame();
    }
  }
});

process.stdout.write("\x1b[2J\x1b[H\x1b[?25l");
frame();
const timer = setInterval(() => {
  ROOT_ELEMENT.state.setState(() => {
    ROOT_ELEMENT.state.tick++;
  });
  frame();
}, 120);
