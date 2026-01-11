import { test, summary } from './helpers.js';
import * as readline from 'node:readline';
import * as readlinePromises from 'node:readline/promises';

console.log('Readline Tests\n');

test('readline has createInterface', typeof readline.createInterface, 'function');
test('readline has clearLine', typeof readline.clearLine, 'function');
test('readline has clearScreenDown', typeof readline.clearScreenDown, 'function');
test('readline has cursorTo', typeof readline.cursorTo, 'function');
test('readline has moveCursor', typeof readline.moveCursor, 'function');
test('readline has emitKeypressEvents', typeof readline.emitKeypressEvents, 'function');

test('readline/promises has createInterface', typeof readlinePromises.createInterface, 'function');

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  prompt: 'test> ',
  historySize: 50,
  removeHistoryDuplicates: true,
  tabSize: 4
});

test('interface is object', typeof rl, 'object');
test('interface has on method', typeof rl.on, 'function');
test('interface has once method', typeof rl.once, 'function');
test('interface has off method', typeof rl.off, 'function');
test('interface has emit method', typeof rl.emit, 'function');
test('interface has close method', typeof rl.close, 'function');
test('interface has pause method', typeof rl.pause, 'function');
test('interface has resume method', typeof rl.resume, 'function');
test('interface has prompt method', typeof rl.prompt, 'function');
test('interface has setPrompt method', typeof rl.setPrompt, 'function');
test('interface has getPrompt method', typeof rl.getPrompt, 'function');
test('interface has write method', typeof rl.write, 'function');
test('interface has question method', typeof rl.question, 'function');
test('interface has getCursorPos method', typeof rl.getCursorPos, 'function');

test('line property is string', typeof rl.line, 'string');
test('line property initially empty', rl.line, '');

test('cursor property is number', typeof rl.cursor, 'number');
test('cursor property initially 0', rl.cursor, 0);

test('closed property is boolean', typeof rl.closed, 'boolean');
test('closed property initially false', rl.closed, false);

const cursorPos = rl.getCursorPos();
test('getCursorPos returns object', typeof cursorPos, 'object');
test('getCursorPos has rows', typeof cursorPos.rows, 'number');
test('getCursorPos has cols', typeof cursorPos.cols, 'number');

test('getPrompt returns initial prompt', rl.getPrompt(), 'test> ');

rl.setPrompt('new> ');
test('setPrompt updates prompt', rl.getPrompt(), 'new> ');

test('terminal property exists', typeof rl.terminal, 'boolean');
test('has Symbol.asyncIterator', typeof rl[Symbol.asyncIterator], 'function');

let closeEventFired = false;
rl.on('close', () => {
  closeEventFired = true;
});

let pauseEventFired = false;
rl.on('pause', () => {
  pauseEventFired = true;
});

let resumeEventFired = false;
rl.on('resume', () => {
  resumeEventFired = true;
});

rl.pause();
test('pause event fired', pauseEventFired, true);

rl.resume();
test('resume event fired', resumeEventFired, true);

rl.close();
test('close event fired', closeEventFired, true);
test('closed property true after close', rl.closed, true);

const clearLineResult = readline.clearLine(process.stdout, 0);
test('clearLine returns boolean', typeof clearLineResult, 'boolean');

const cursorToResult = readline.cursorTo(process.stdout, 0);
test('cursorTo returns boolean', typeof cursorToResult, 'boolean');

const moveCursorResult = readline.moveCursor(process.stdout, 0, 0);
test('moveCursor returns boolean', typeof moveCursorResult, 'boolean');

const rlPromises = readlinePromises.createInterface({
  input: process.stdin,
  output: process.stdout
});

test('promises interface is object', typeof rlPromises, 'object');
test('promises interface has question', typeof rlPromises.question, 'function');
rlPromises.close();

const rlDefault = readline.createInterface({
  input: process.stdin,
  output: process.stdout
});
test('default prompt is "> "', rlDefault.getPrompt(), '> ');
rlDefault.close();

const rlHistory = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  history: ['cmd1', 'cmd2', 'cmd3']
});
test('history interface created', typeof rlHistory, 'object');
rlHistory.close();

const rl2 = readline.createInterface({
  input: process.stdin,
  output: process.stdout
});

test('addListener is function', typeof rl2.addListener, 'function');
test('removeListener is function', typeof rl2.removeListener, 'function');

let testEventFired = false;
const testHandler = () => {
  testEventFired = true;
};
rl2.addListener('test', testHandler);
rl2.emit('test');
test('addListener works', testEventFired, true);

testEventFired = false;
rl2.removeListener('test', testHandler);
rl2.emit('test');
test('removeListener works', testEventFired, false);

rl2.close();

const rl3 = readline.createInterface({
  input: process.stdin,
  output: process.stdout
});

let onceCount = 0;
rl3.once('myevent', () => {
  onceCount++;
});
rl3.emit('myevent');
rl3.emit('myevent');
test('once only fires once', onceCount, 1);
rl3.close();

const rl4 = readline.createInterface({
  input: process.stdin,
  output: process.stdout
});

const chainResult = rl4.on('test', () => {});
test('on returns interface for chaining', chainResult, rl4);
rl4.close();

const rl5 = readline.createInterface({
  input: process.stdin,
  output: process.stdout
});

rl5.write('hello');
test('write updates line', rl5.line, 'hello');
test('write updates cursor', rl5.cursor, 5);

rl5.write(null, { name: 'backspace' });
test('backspace key works', rl5.line, 'hell');
test('backspace updates cursor', rl5.cursor, 4);

rl5.write(null, { name: 'left' });
test('left arrow updates cursor', rl5.cursor, 3);

rl5.write(null, { name: 'right' });
test('right arrow updates cursor', rl5.cursor, 4);

rl5.write(null, { name: 'home' });
test('home key moves cursor to 0', rl5.cursor, 0);

rl5.write(null, { name: 'end' });
test('end key moves cursor to end', rl5.cursor, 4);

rl5.write(null, { name: 'u', ctrl: true });
test('ctrl+u clears line', rl5.line, '');

rl5.close();

summary();
