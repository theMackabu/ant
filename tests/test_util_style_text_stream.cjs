const assert = require('node:assert');
const { Writable } = require('node:stream');
const { styleText } = require('node:util');

const colored = '\u001b[90mhi\u001b[39m';
const stream = props => Object.assign(new Writable({ write: (_chunk, _enc, cb) => cb() }), props);
const tty = depth => stream({ isTTY: true, getColorDepth: () => depth });
const pipe = stream({ isTTY: false });

assert.strictEqual(styleText('gray', 'hi', { validateStream: false }), colored);
assert.strictEqual(styleText('gray', 'hi', { stream: pipe, validateStream: false }), colored);

if (process.env.FORCE_COLOR === undefined) {
  assert.strictEqual(styleText('gray', 'hi', { stream: pipe }), 'hi');
  assert.strictEqual(styleText('gray', 'hi', { stream: tty(8) }), colored);
  assert.strictEqual(styleText('gray', 'hi', { stream: tty(1) }), 'hi');
  assert.strictEqual(styleText('gray', 'hi', { stream: stream({ isTTY: true }) }), colored);
} else if (process.env.FORCE_COLOR === '0') {
  assert.strictEqual(styleText('gray', 'hi', { stream: tty(8) }), 'hi');
} else {
  assert.strictEqual(styleText('gray', 'hi', { stream: pipe }), colored);
}

assert.strictEqual(styleText([], 'hi', { validateStream: false }), 'hi');

console.log('styleText stream handling: ok');
