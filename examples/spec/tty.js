import { test, testThrows, summary } from './helpers.js';
import * as tty from 'node:tty';

console.log('TTY Tests\n');

test('tty module exists', typeof tty, 'object');
test('tty toStringTag', Object.prototype.toString.call(tty), '[object tty]');
test('tty.isatty is function', typeof tty.isatty, 'function');
test('tty.ReadStream is function', typeof tty.ReadStream, 'function');
test('tty.WriteStream is function', typeof tty.WriteStream, 'function');

test('tty.isatty(-1) is false', tty.isatty(-1), false);
test('tty.isatty(0) is boolean', typeof tty.isatty(0), 'boolean');
test('tty.isatty(1) is boolean', typeof tty.isatty(1), 'boolean');
test('tty.isatty(2) is boolean', typeof tty.isatty(2), 'boolean');

test('stdin has fd', typeof process.stdin.fd, 'number');
test('stdout has fd', typeof process.stdout.fd, 'number');
test('stderr has fd', typeof process.stderr.fd, 'number');

test('stdin toStringTag', Object.prototype.toString.call(process.stdin), '[object ReadStream]');
test('stdout toStringTag', Object.prototype.toString.call(process.stdout), '[object WriteStream]');
test('stderr toStringTag', Object.prototype.toString.call(process.stderr), '[object WriteStream]');

test('stdin.setRawMode is function', typeof process.stdin.setRawMode, 'function');
test('stdout.clearLine is function', typeof process.stdout.clearLine, 'function');
test('stdout.clearScreenDown is function', typeof process.stdout.clearScreenDown, 'function');
test('stdout.cursorTo is function', typeof process.stdout.cursorTo, 'function');
test('stdout.moveCursor is function', typeof process.stdout.moveCursor, 'function');
test('stdout.getWindowSize is function', typeof process.stdout.getWindowSize, 'function');
test('stdout.getColorDepth is function', typeof process.stdout.getColorDepth, 'function');
test('stdout.hasColors is function', typeof process.stdout.hasColors, 'function');

const ws = process.stdout.getWindowSize();
test('getWindowSize returns array', Array.isArray(ws), true);
test('getWindowSize length is 2', ws.length, 2);
test('getWindowSize cols is number', typeof ws[0], 'number');
test('getWindowSize rows is number', typeof ws[1], 'number');

const depthDefault = process.stdout.getColorDepth();
test('getColorDepth returns number', typeof depthDefault, 'number');
test('getColorDepth returns valid depth', [1, 4, 8, 24].includes(depthDefault), true);

const depthForced = process.stdout.getColorDepth({ FORCE_COLOR: '3' });
test('getColorDepth(force truecolor) = 24', depthForced, 24);
const depthNoColor = process.stdout.getColorDepth({ NO_COLOR: '1' });
test('getColorDepth(NO_COLOR) = 1', depthNoColor, 1);

test('hasColors() returns boolean', typeof process.stdout.hasColors(), 'boolean');
test('hasColors(force truecolor) supports 16m', process.stdout.hasColors(16777216, { FORCE_COLOR: '3' }), true);
test('hasColors(NO_COLOR) rejects 16 colors', process.stdout.hasColors(16, { NO_COLOR: '1' }), false);

const clearLineResult = process.stdout.clearLine(0);
test('clearLine returns stream', clearLineResult, process.stdout);
const clearDownResult = process.stdout.clearScreenDown();
test('clearScreenDown returns stream', clearDownResult, process.stdout);
const cursorToResult = process.stdout.cursorTo(0);
test('cursorTo returns stream', cursorToResult, process.stdout);
const moveCursorResult = process.stdout.moveCursor(0, 0);
test('moveCursor returns stream', moveCursorResult, process.stdout);

if (process.stdin.isTTY) {
  const stdinCtor = new tty.ReadStream(0);
  test('new ReadStream(0) returns stdin-like stream', Object.prototype.toString.call(stdinCtor), '[object ReadStream]');
  test('ReadStream constructor reuses stdin stream', stdinCtor === process.stdin, true);

  const rawResult = process.stdin.setRawMode(false);
  test('stdin.setRawMode returns this', rawResult, process.stdin);
  test('stdin.isRaw is boolean', typeof process.stdin.isRaw, 'boolean');
} else {
  testThrows('new ReadStream(0) throws when stdin is not TTY', () => new tty.ReadStream(0));
}

if (process.stdout.isTTY) {
  const stdoutCtor = new tty.WriteStream(1);
  test('new WriteStream(1) returns stdout-like stream', Object.prototype.toString.call(stdoutCtor), '[object WriteStream]');
  test('WriteStream constructor reuses stdout stream', stdoutCtor === process.stdout, true);
} else {
  testThrows('new WriteStream(1) throws when stdout is not TTY', () => new tty.WriteStream(1));
}

if (process.stderr.isTTY) {
  const stderrCtor = new tty.WriteStream(2);
  test('new WriteStream(2) returns stderr-like stream', Object.prototype.toString.call(stderrCtor), '[object WriteStream]');
  test('WriteStream constructor reuses stderr stream', stderrCtor === process.stderr, true);
} else {
  testThrows('new WriteStream(2) throws when stderr is not TTY', () => new tty.WriteStream(2));
}

summary();
