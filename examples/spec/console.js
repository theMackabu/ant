import consoleModule, { Console } from 'node:console';
import { test, testDeep, summary } from './helpers.js';

console.log('Console Tests\n');

test('console.log exists', typeof console.log, 'function');
test('console.error exists', typeof console.error, 'function');
test('console.warn exists', typeof console.warn, 'function');
test('console.info exists', typeof console.info, 'function');
test('console.debug exists', typeof console.debug, 'function');
test('console.trace exists', typeof console.trace, 'function');
test('console.time exists', typeof console.time, 'function');
test('console.timeEnd exists', typeof console.timeEnd, 'function');
test('console.timeLog exists', typeof console.timeLog, 'function');
test('console.assert exists', typeof console.assert, 'function');
test('console.clear exists', typeof console.clear, 'function');
test('console.count exists', typeof console.count, 'function');
test('console.countReset exists', typeof console.countReset, 'function');
test('console.group exists', typeof console.group, 'function');
test('console.groupCollapsed exists', typeof console.groupCollapsed, 'function');
test('console.groupEnd exists', typeof console.groupEnd, 'function');
test('console.dir exists', typeof console.dir, 'function');
test('console.dirxml exists', typeof console.dirxml, 'function');
test('console.table exists', typeof console.table, 'function');
test('console.Console exists', typeof console.Console, 'function');

test('node:console default export is object', typeof consoleModule, 'object');
test('node:console Console export exists', typeof Console, 'function');
test('node:console default Console matches named export', consoleModule.Console, Console);

function makeSink() {
  return {
    chunks: [],
    write(chunk) {
      this.chunks.push(String(chunk));
      return true;
    }
  };
}

const stdoutSink = makeSink();
const stderrSink = makeSink();
const instance = new Console({ stdout: stdoutSink, stderr: stderrSink, groupIndentation: 2 });

test('Console instance log exists', typeof instance.log, 'function');
test('Console instance error exists', typeof instance.error, 'function');
test('Console instance warn exists', typeof instance.warn, 'function');
test('Console instance info exists', typeof instance.info, 'function');
test('Console instance debug exists', typeof instance.debug, 'function');
test('Console instance assert exists', typeof instance.assert, 'function');
test('Console instance trace exists', typeof instance.trace, 'function');
test('Console instance count exists', typeof instance.count, 'function');
test('Console instance countReset exists', typeof instance.countReset, 'function');
test('Console instance time exists', typeof instance.time, 'function');
test('Console instance timeLog exists', typeof instance.timeLog, 'function');
test('Console instance timeEnd exists', typeof instance.timeEnd, 'function');
test('Console instance group exists', typeof instance.group, 'function');
test('Console instance groupCollapsed exists', typeof instance.groupCollapsed, 'function');
test('Console instance groupEnd exists', typeof instance.groupEnd, 'function');
test('Console instance clear exists', typeof instance.clear, 'function');
test('Console instance dir exists', typeof instance.dir, 'function');
test('Console instance dirxml exists', typeof instance.dirxml, 'function');
test('Console instance table exists', typeof instance.table, 'function');

instance.log('hello', 'world');
instance.info('info');
instance.group('parent');
instance.log('child');
instance.groupEnd();
instance.count('hits');
instance.count('hits');
instance.countReset('hits');
instance.count('hits');
instance.error('problem');
instance.warn('warning');
instance.assert(false, 'boom');

testDeep('Console instance stdout routing', stdoutSink.chunks, [
  'hello world\n',
  'info\n',
  'parent\n',
  '  child\n',
  'hits: 1\n',
  'hits: 2\n',
  'hits: 1\n'
]);

testDeep('Console instance stderr routing', stderrSink.chunks, [
  'problem\n',
  'warning\n',
  'Assertion failed: boom\n'
]);

summary();
