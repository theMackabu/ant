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
  mouseOn: `${CSI}?1000h${CSI}?1006h`,
  mouseOff: `${CSI}?1000l${CSI}?1006l`
};

const state = {
  x: 0,
  y: 0,
  button: 'none',
  action: 'move',
  lastEvent: ''
};

function write(text) {
  process.stdout.write(text);
}

function render() {
  const width = process.stdout.columns || 80;
  const height = process.stdout.rows || 24;
  const lines = [];

  lines.push(' Simple Mouse TUI');
  lines.push('');
  lines.push(` Position: ${state.x}, ${state.y}`);
  lines.push(` Button:   ${state.button}`);
  lines.push(` Action:   ${state.action}`);
  lines.push('');
  lines.push(' Last event:');
  lines.push(` ${state.lastEvent || 'none'}`);
  lines.push('');
  lines.push(' Move or click in the terminal. Press q or Ctrl-C to exit.');

  while (lines.length < height) {
    lines.push('');
  }

  const padded = lines.map(line => {
    if (line.length >= width) return line.slice(0, width);
    return line + ' '.repeat(width - line.length);
  });

  write(codes.home + codes.clear + padded.join('\n') + codes.reset);
}

function parseMouseEvent(seq) {
  const match = seq.match(/^\x1b\[<([0-9]+);([0-9]+);([0-9]+)([mM])$/);
  if (!match) return false;

  const [, codeStr, xStr, yStr, action] = match;
  const code = Number(codeStr);
  const x = Number(xStr);
  const y = Number(yStr);
  const buttonCode = code & 3;

  let button = 'none';
  if (buttonCode === 0) button = 'left';
  if (buttonCode === 1) button = 'middle';
  if (buttonCode === 2) button = 'right';

  let actionLabel = 'move';
  if (action === 'M' && (code & 32) === 0) actionLabel = 'press';
  if (action === 'm') actionLabel = 'release';

  state.x = x;
  state.y = y;
  state.button = button;
  state.action = actionLabel;
  state.lastEvent = `code=${code} x=${x} y=${y} action=${action}`;
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
      return;
    }
  }
}

function cleanup() {
  process.stdin.setRawMode(false);
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
