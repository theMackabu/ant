import {
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
  List,
  ProgressBar,
  Input,
  Table
} from './tuey.js';

let passed = 0;
let failed = 0;

function test(name, fn) {
  try {
    fn();
    console.log(`${colors.green}✓${colors.reset} ${name}`);
    passed++;
  } catch (e) {
    console.log(`${colors.red}✗${colors.reset} ${name}`);
    console.log(`  ${colors.dim}${e.message}${colors.reset}`);
    failed++;
  }
}

function assert(condition, message) {
  if (!condition) throw new Error(message || 'Assertion failed');
}

function assertEqual(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(message || `Expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

console.log(`${colors.bold}${colors.blue}═══════════════════════════════════════${colors.reset}`);
console.log(`${colors.bold}  TUI Library Tests${colors.reset}`);
console.log(`${colors.bold}${colors.blue}═══════════════════════════════════════${colors.reset}\n`);

console.log(`${colors.cyan}ANSI Code Generation${colors.reset}`);

test('codes.fg generates correct 256-color code', () => {
  assertEqual(codes.fg(196), '\x1b[38;5;196m');
});

test('codes.bg generates correct 256-color code', () => {
  assertEqual(codes.bg(24), '\x1b[48;5;24m');
});

test('codes.rgb generates correct 24-bit color code', () => {
  assertEqual(codes.rgb(255, 128, 0), '\x1b[38;2;255;128;0m');
});

test('codes.moveTo generates correct cursor position', () => {
  assertEqual(codes.moveTo(10, 5), '\x1b[6;11H');
});

console.log(`\n${colors.cyan}String Utilities${colors.reset}`);

test('stripAnsi removes ANSI codes', () => {
  const input = `${colors.red}hello${colors.reset} ${colors.bold}world${colors.reset}`;
  assertEqual(stripAnsi(input), 'hello world');
});

test('visibleLength calculates correct length with ANSI', () => {
  const input = `${colors.green}test${colors.reset}`;
  assertEqual(visibleLength(input), 4);
});

test('pad right-pads to target length', () => {
  assertEqual(pad('hello', 10), 'hello     ');
});

test('pad handles strings longer than target', () => {
  assertEqual(pad('hello world', 5), 'hello world');
});

test('pad works with ANSI codes', () => {
  const input = `${colors.red}hi${colors.reset}`;
  const result = pad(input, 5);
  assertEqual(visibleLength(result), 5);
  assert(result.includes('\x1b[38;5;196m'), 'Should contain red color code');
});

test('padStart left-pads to target length', () => {
  assertEqual(padStart('123', 6), '   123');
});

test('padCenter centers text', () => {
  assertEqual(padCenter('hi', 6), '  hi  ');
});

test('truncate shortens long strings', () => {
  const result = truncate('hello world', 8, '.');
  assertEqual(stripAnsi(result), 'hello w.');
});

test('truncate preserves short strings', () => {
  assertEqual(truncate('hi', 10), 'hi');
});

test('truncate handles ANSI codes correctly', () => {
  const input = `${colors.red}hello world${colors.reset}`;
  const result = truncate(input, 8);
  assertEqual(visibleLength(result), 8);
});

test('wrap splits text at word boundaries', () => {
  const result = wrap('hello world foo bar', 12);
  assertEqual(result.length, 2);
  assertEqual(result[0], 'hello world');
});

console.log(`\n${colors.cyan}Buffer${colors.reset}`);

test('Buffer initializes with correct dimensions', () => {
  const buf = new Buffer(20, 10);
  assertEqual(buf.width, 20);
  assertEqual(buf.height, 10);
  assertEqual(buf.lines.length, 10);
});

test('Buffer.write writes text at position', () => {
  const buf = new Buffer(20, 5);
  buf.write(5, 2, 'hello');
  assert(buf.lines[2].includes('hello'), 'Buffer should contain "hello"');
});

test('Buffer.clear resets all lines', () => {
  const buf = new Buffer(10, 5);
  buf.write(0, 0, 'test');
  buf.clear();
  assertEqual(buf.lines[0], ' '.repeat(10));
});

test('Buffer.resize changes dimensions', () => {
  const buf = new Buffer(10, 5);
  buf.write(0, 0, 'hello');
  buf.resize(20, 10);
  assertEqual(buf.width, 20);
  assertEqual(buf.height, 10);
  assert(buf.lines[0].includes('hello'), 'Content should be preserved');
});

test('Buffer.clone creates independent copy', () => {
  const buf = new Buffer(10, 5);
  buf.write(0, 0, 'original');
  const clone = buf.clone();
  buf.write(0, 0, 'modified');
  assert(clone.lines[0].includes('original'), 'Clone should be independent');
});

test('Buffer.fill fills rectangular region', () => {
  const buf = new Buffer(10, 5);
  buf.fill(2, 1, 3, 2, 'X');
  assert(buf.lines[1].includes('XXX'), 'Should contain filled region');
  assert(buf.lines[2].includes('XXX'), 'Should contain filled region');
});

test('Buffer.box draws box characters', () => {
  const buf = new Buffer(20, 10);
  buf.box(0, 0, 10, 5, box.light);
  assert(buf.lines[0].includes('┌'), 'Should have top-left corner');
  assert(buf.lines[0].includes('┐'), 'Should have top-right corner');
  assert(buf.lines[4].includes('└'), 'Should have bottom-left corner');
});

console.log(`\n${colors.cyan}List${colors.reset}`);

test('List initializes with items', () => {
  const list = new List({
    items: ['a', 'b', 'c'],
    width: 20,
    height: 5
  });
  assertEqual(list.items.length, 3);
  assertEqual(list.index, 0);
});

test('List.selectNext advances selection', () => {
  const list = new List({ items: ['a', 'b', 'c'] });
  list.selectNext();
  assertEqual(list.index, 1);
});

test('List.selectPrev moves selection back', () => {
  const list = new List({ items: ['a', 'b', 'c'], index: 2 });
  list.selectPrev();
  assertEqual(list.index, 1);
});

test('List.selectNext clamps at end', () => {
  const list = new List({ items: ['a', 'b', 'c'], index: 2 });
  list.selectNext();
  assertEqual(list.index, 2);
});

test('List.selectPrev clamps at start', () => {
  const list = new List({ items: ['a', 'b', 'c'], index: 0 });
  list.selectPrev();
  assertEqual(list.index, 0);
});

test('List.getSelected returns current item', () => {
  const list = new List({ items: ['a', 'b', 'c'], index: 1 });
  assertEqual(list.getSelected(), 'b');
});

test('List.setItems updates and clamps index', () => {
  const list = new List({ items: ['a', 'b', 'c', 'd'], index: 3 });
  list.setItems(['x', 'y']);
  assertEqual(list.items.length, 2);
  assertEqual(list.index, 1);
});

test('List.handleKey responds to vim keys', () => {
  const list = new List({ items: ['a', 'b', 'c'] });
  list.handleKey('j');
  assertEqual(list.index, 1);
  list.handleKey('k');
  assertEqual(list.index, 0);
});

test('List.handleKey responds to G/g', () => {
  const list = new List({ items: ['a', 'b', 'c', 'd', 'e'] });
  list.handleKey('G');
  assertEqual(list.index, 4);
  list.handleKey('g');
  assertEqual(list.index, 0);
});

console.log(`\n${colors.cyan}ProgressBar${colors.reset}`);

test('ProgressBar initializes with value', () => {
  const bar = new ProgressBar({ value: 50, max: 100 });
  assertEqual(bar.value, 50);
  assertEqual(bar.max, 100);
});

test('ProgressBar.setValue clamps value', () => {
  const bar = new ProgressBar({ max: 100 });
  bar.setValue(150);
  assertEqual(bar.value, 100);
  bar.setValue(-10);
  assertEqual(bar.value, 0);
});

test('ProgressBar.render produces filled/empty chars', () => {
  const bar = new ProgressBar({ value: 50, max: 100, width: 10, showPercent: false });
  const result = bar.render();
  const stripped = stripAnsi(result);
  assert(stripped.includes('█'), 'Should have filled chars');
  assert(stripped.includes('░'), 'Should have empty chars');
});

test('ProgressBar.render shows percentage', () => {
  const bar = new ProgressBar({ value: 75, max: 100, width: 10, showPercent: true });
  const result = bar.render();
  assert(result.includes('75%'), 'Should show percentage');
});

console.log(`\n${colors.cyan}Input${colors.reset}`);

test('Input initializes with value', () => {
  const input = new Input({ value: 'hello' });
  assertEqual(input.value, 'hello');
  assertEqual(input.cursorPos, 5);
});

test('Input.handleKey adds characters', () => {
  const input = new Input();
  input.handleKey('a');
  input.handleKey('b');
  input.handleKey('c');
  assertEqual(input.value, 'abc');
});

test('Input.handleKey handles backspace', () => {
  const input = new Input({ value: 'hello' });
  input.handleKey(keys.BACKSPACE);
  assertEqual(input.value, 'hell');
});

test('Input.handleKey handles arrow keys', () => {
  const input = new Input({ value: 'hello' });
  input.handleKey(keys.LEFT);
  assertEqual(input.cursorPos, 4);
  input.handleKey(keys.RIGHT);
  assertEqual(input.cursorPos, 5);
});

test('Input.clear resets value and cursor', () => {
  const input = new Input({ value: 'test' });
  input.clear();
  assertEqual(input.value, '');
  assertEqual(input.cursorPos, 0);
});

console.log(`\n${colors.cyan}Table${colors.reset}`);

test('Table initializes with columns and rows', () => {
  const table = new Table({
    columns: [
      { key: 'name', header: 'Name' },
      { key: 'value', header: 'Value' }
    ],
    rows: [{ name: 'foo', value: '123' }]
  });
  assertEqual(table.columns.length, 2);
  assertEqual(table.rows.length, 1);
});

console.log(`\n${colors.cyan}Box Drawing Styles${colors.reset}`);

test('box.light has correct characters', () => {
  assertEqual(box.light.tl, '┌');
  assertEqual(box.light.tr, '┐');
  assertEqual(box.light.h, '─');
  assertEqual(box.light.v, '│');
});

test('box.double has correct characters', () => {
  assertEqual(box.double.tl, '╔');
  assertEqual(box.double.tr, '╗');
  assertEqual(box.double.h, '═');
  assertEqual(box.double.v, '║');
});

test('box.rounded has correct characters', () => {
  assertEqual(box.rounded.tl, '╭');
  assertEqual(box.rounded.br, '╯');
});

console.log(`\n${colors.cyan}Keys Constants${colors.reset}`);

test('keys.UP is correct escape sequence', () => {
  assertEqual(keys.UP, '\x1b[A');
});

test('keys.ESCAPE is escape character', () => {
  assertEqual(keys.ESCAPE, '\x1b');
});

test('keys.ENTER is carriage return', () => {
  assertEqual(keys.ENTER, '\r');
});

console.log(`\n${colors.blue}═══════════════════════════════════════${colors.reset}`);
const total = passed + failed;
const rate = ((passed / total) * 100).toFixed(1);
const rateColor = failed === 0 ? colors.green : colors.yellow;

console.log(
  `${colors.bold}Results:${colors.reset} ${colors.green}${passed} passed${colors.reset}, ${failed > 0 ? colors.red : colors.dim}${failed} failed${colors.reset}`
);
console.log(`${colors.bold}Rate:${colors.reset} ${rateColor}${rate}%${colors.reset}`);
console.log(`${colors.blue}═══════════════════════════════════════${colors.reset}`);

if (failed > 0) process.exit(1);
