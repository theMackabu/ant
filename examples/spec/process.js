import { test, summary } from './helpers.js';

console.log('Process Tests\n');

test('process exists', typeof process, 'object');
test('process toStringTag', Object.prototype.toString.call(process), '[object process]');

test('pid is number', typeof process.pid, 'number');
test('pid is positive', process.pid > 0, true);

test('platform is string', typeof process.platform, 'string');
test('platform valid', ['darwin', 'linux', 'win32', 'freebsd'].includes(process.platform), true);

test('arch is string', typeof process.arch, 'string');
test('arch valid', ['x64', 'ia32', 'arm64', 'arm'].includes(process.arch), true);

test('argv is array', Array.isArray(process.argv), true);
test('argv has entries', process.argv.length >= 1, true);

test('cwd is function', typeof process.cwd, 'function');
const cwd = process.cwd();
test('cwd returns string', typeof cwd, 'string');
test('cwd not empty', cwd.length > 0, true);

test('env is object', typeof process.env, 'object');
test('env has PATH or Path', process.env.PATH !== undefined || process.env.Path !== undefined, true);

test('exit is function', typeof process.exit, 'function');

test('on is function', typeof process.on, 'function');
test('once is function', typeof process.once, 'function');
test('off is function', typeof process.off, 'function');
test('addListener is function', typeof process.addListener, 'function');
test('removeListener is function', typeof process.removeListener, 'function');
test('removeAllListeners is function', typeof process.removeAllListeners, 'function');
test('emit is function', typeof process.emit, 'function');
test('listenerCount is function', typeof process.listenerCount, 'function');
test('setMaxListeners is function', typeof process.setMaxListeners, 'function');
test('getMaxListeners is function', typeof process.getMaxListeners, 'function');

let onCalled = false;
process.on('testOn', () => {
  onCalled = true;
});
process.emit('testOn');
test('on/emit works', onCalled, true);
process.removeAllListeners('testOn');

let onceCount = 0;
process.once('testOnce', () => {
  onceCount++;
});
process.emit('testOnce');
process.emit('testOnce');
test('once fires once', onceCount, 1);

let offCount = 0;
const offHandler = () => {
  offCount++;
};
process.on('testOff', offHandler);
process.emit('testOff');
process.off('testOff', offHandler);
process.emit('testOff');
test('off removes listener', offCount, 1);

let addListenerCalled = false;
process.addListener('testAddListener', () => {
  addListenerCalled = true;
});
process.emit('testAddListener');
test('addListener works', addListenerCalled, true);
process.removeAllListeners('testAddListener');

let removeListenerCount = 0;
const rlHandler = () => {
  removeListenerCount++;
};
process.on('testRL', rlHandler);
process.emit('testRL');
process.removeListener('testRL', rlHandler);
process.emit('testRL');
test('removeListener works', removeListenerCount, 1);

process.on('testCount', () => {});
process.on('testCount', () => {});
test('listenerCount returns count', process.listenerCount('testCount'), 2);
process.removeAllListeners('testCount');
test('removeAllListeners clears', process.listenerCount('testCount'), 0);

let receivedArg = null;
process.on('testArgs', arg => {
  receivedArg = arg;
});
process.emit('testArgs', 'hello');
test('emit passes arguments', receivedArg, 'hello');
process.removeAllListeners('testArgs');

const chainResult = process.on('testChain', () => {});
test('on returns process', chainResult, process);
process.removeAllListeners('testChain');

test('getMaxListeners default', process.getMaxListeners(), 10);

process.setMaxListeners(20);
test('setMaxListeners updates', process.getMaxListeners(), 20);
process.setMaxListeners(10);

process.setMaxListeners(0);
for (let i = 0; i < 15; i++) {
  process.on('manyListeners', () => {});
}
test('supports many listeners', process.listenerCount('manyListeners'), 15);
process.removeAllListeners('manyListeners');
process.setMaxListeners(10);

summary();
