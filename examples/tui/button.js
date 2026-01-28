const ESC = '\x1b';
const CSI = `${ESC}[`;

const codes = {
  altScreenOn: `${CSI}?1049h`,
  altScreenOff: `${CSI}?1049l`,
  hideCursor: `${CSI}?25l`,
  showCursor: `${CSI}?25h`,
  clear: `${CSI}2J`,
  home: `${CSI}H`,
  reset: `${CSI}0m`,
  mouseOn: `${CSI}?1000h${CSI}?1003h${CSI}?1006h`,
  mouseOff: `${CSI}?1000l${CSI}?1003l${CSI}?1006l`,
  invert: `${CSI}7m`,
  fg: n => `${CSI}38;5;${n}m`,
  bg: n => `${CSI}48;5;${n}m`
};

const state = {
  hover: false,
  pressed: false,
  message: 'Hover or click the button'
};

const button = {
  label: 'Click Me',
  x: 0,
  y: 0,
  width: 0,
  height: 1
};

const styles = {
  normal: '',
  hover: `${codes.bg(46)}${codes.fg(16)}`,
  active: `${codes.bg(196)}${codes.fg(15)}`
};

function write(text) {
  process.stdout.write(text);
}

function centerButton(width, height) {
  button.width = button.label.length + 4;
  button.x = Math.max(0, Math.floor((width - button.width) / 2));
  button.y = Math.max(0, Math.floor((height - button.height) / 2));
}

function createGrid(width, height) {
  const chars = Array(height).fill(null).map(() => Array(width).fill(' '));
  const stylesGrid = Array(height).fill(null).map(() => Array(width).fill(''));
  return { chars, styles: stylesGrid };
}

function setCell(grid, x, y, ch, style) {
  if (y < 0 || y >= grid.chars.length) return;
  if (x < 0 || x >= grid.chars[y].length) return;
  grid.chars[y][x] = ch;
  grid.styles[y][x] = style;
}

function setText(grid, x, y, text, style) {
  for (let i = 0; i < text.length; i++) {
    setCell(grid, x + i, y, text[i], style);
  }
}

function drawButton(grid) {
  const fillStyle = state.pressed
    ? styles.active
    : state.hover
      ? styles.hover
      : styles.normal;
  for (let row = 0; row < button.height; row++) {
    for (let col = 0; col < button.width; col++) {
      setCell(grid, button.x + col, button.y + row, ' ', fillStyle);
    }
  }

  const padTotal = Math.max(0, button.width - button.label.length);
  const padLeft = Math.floor(padTotal / 2);
  const labelX = button.x + padLeft;
  const labelY = button.y + Math.floor(button.height / 2);
  setText(grid, labelX, labelY, button.label, fillStyle);
}

function render() {
  const width = process.stdout.columns || 80;
  const height = process.stdout.rows || 24;
  centerButton(width, height);

  const grid = createGrid(width, height);
  const header = ' Simple Button TUI (q to quit)';
  setText(grid, 0, 0, header.slice(0, width), '');

  const message = state.message;
  const messageLine = Math.min(height - 2, button.y + button.height + 1);
  if (messageLine >= 0 && messageLine < height) {
    setText(grid, 1, messageLine, message.slice(0, width - 2), '');
  }

  drawButton(grid);
  const rows = grid.chars.map((row, y) => {
    let line = '';
    let currentStyle = '';
    for (let x = 0; x < row.length; x++) {
      const nextStyle = grid.styles[y][x];
      if (nextStyle !== currentStyle) {
        line += nextStyle || codes.reset;
        currentStyle = nextStyle;
      }
      line += row[x];
    }
    return line + codes.reset;
  });
  write(codes.home + codes.clear + rows.join('\n') + codes.reset);
}

function isInsideButton(x, y) {
  return (
    x >= button.x &&
    x < button.x + button.width &&
    y >= button.y &&
    y < button.y + button.height
  );
}

function parseMouseEvent(seq) {
  const match = seq.match(/^\x1b\[<([0-9]+);([0-9]+);([0-9]+)([mM])$/);
  if (!match) return false;

  const [, codeStr, xStr, yStr, action] = match;
  const code = Number(codeStr);
  const x = Number(xStr) - 1;
  const y = Number(yStr) - 1;
  const buttonCode = code & 3;

  const inside = isInsideButton(x, y);
  const prevHover = state.hover;
  state.hover = inside;

  if (action === 'M' && buttonCode === 0 && inside) {
    state.pressed = true;
    state.message = 'Button pressed! Release to click.';
  } else if (action === 'm') {
    if (state.pressed && inside) {
      state.message = 'Button clicked!';
    }
    state.pressed = false;
  }

  if (inside && !prevHover && action === 'M' && (code & 32)) {
    state.message = 'Hovering on the button.';
  }

  if (!inside && prevHover && action === 'M' && (code & 32)) {
    state.message = 'Hover or click the button';
  }

  return true;
}

function handleInput(chunk) {
  const str = chunk.toString();
  if (str === 'q' || str === '\x03') {
    cleanup();
    return;
  }

  if (str.startsWith('\x1b[<')) {
    if (parseMouseEvent(str)) {
      render();
    }
  }
}

function cleanup() {
  if (process.stdin.isTTY) {
    process.stdin.setRawMode(false);
  }
  process.stdin.pause();
  process.stdin.removeListener('data', handleInput);
  process.stdout.removeListener('resize', render);
  write(codes.mouseOff + codes.showCursor + codes.altScreenOff);
  process.exit(0);
}

function start() {
  if (process.stdin.isTTY) {
    process.stdin.setRawMode(true);
  }
  process.stdin.resume();
  process.stdin.on('data', handleInput);
  process.stdout.on('resize', render);

  write(codes.altScreenOn + codes.hideCursor + codes.mouseOn);
  render();
}

process.on('SIGINT', cleanup);
process.on('SIGTERM', cleanup);

start();
