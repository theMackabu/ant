import { test, summary } from './helpers.js';
import { $ } from 'ant:shell';

console.log('Shell Tests\n');

const result = $`echo hello`;
test('shell returns object', typeof result, 'object');
test('shell has exitCode', typeof result.exitCode, 'number');
test('shell exitCode success', result.exitCode, 0);
test('shell has text method', typeof result.text, 'function');
test('shell has lines method', typeof result.lines, 'function');

test('shell text returns string', typeof result.text(), 'string');
test('shell text content', result.text(), 'hello');

const lines = result.lines();
test('shell lines returns array', Array.isArray(lines), true);
test('shell lines content', lines[0], 'hello');

const multiResult = $`printf "line1\nline2\nline3"`;
const multiLines = multiResult.lines();
test('shell multi-line count', multiLines.length, 3);
test('shell multi-line first', multiLines[0], 'line1');
test('shell multi-line last', multiLines[2], 'line3');

const name = 'world';
const interpResult = $`echo hello ${name}`;
test('shell interpolation', interpResult.text(), 'hello world');

const failResult = $`exit 1`;
test('shell exit code failure', failResult.exitCode, 1);

const strResult = $('echo string arg');
test('shell string arg', strResult.text(), 'string arg');

summary();
