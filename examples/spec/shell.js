import { test, summary } from './helpers.js';
import { $ } from 'ant:shell';

console.log('Shell Tests\n');

const pending = $`echo hello`;
test('shell returns thenable object', typeof pending, 'object');
test('shell has then method', typeof pending.then, 'function');
test('shell has text method', typeof pending.text, 'function');
test('shell has lines method', typeof pending.lines, 'function');
test('shell has nothrow method', typeof pending.nothrow, 'function');

const result = await pending;
test('shell has exitCode', typeof result.exitCode, 'number');
test('shell exitCode success', result.exitCode, 0);
test('shell stdout content', result.stdout, 'hello\n');

test('shell text returns string', typeof (await $`echo hello`.text()), 'string');
test('shell text content', await $`echo hello`.text(), 'hello\n');

const lines = await $`echo hello`.lines();
test('shell lines returns array', Array.isArray(lines), true);
test('shell lines content', lines[0], 'hello');

const multiLines = await $`printf "line1\nline2\nline3"`.lines();
test('shell multi-line count', multiLines.length, 3);
test('shell multi-line first', multiLines[0], 'line1');
test('shell multi-line last', multiLines[2], 'line3');

const name = 'world';
test('shell interpolation', await $`echo hello ${name}`.text(), 'hello world\n');

const failResult = await $`exit 1`.nothrow();
test('shell exit code failure', failResult.exitCode, 1);

let stringCallError;
try {
  $('echo string arg');
} catch (error) {
  stringCallError = error;
}
test('shell rejects string calls', stringCallError instanceof TypeError, true);

summary();
