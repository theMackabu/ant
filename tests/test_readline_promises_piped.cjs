const { spawnSync } = require('child_process');
const path = require('path');

function fail(message) {
  throw new Error(message);
}

const helper = path.join(__dirname, 'fixtures', 'readline_promises_piped_child.cjs');
const result = spawnSync(process.execPath, [helper], {
  input: '42\n',
  encoding: 'utf8',
  timeout: 5000,
});

if (result.error) throw result.error;

const output = `${result.stdout || ''}${result.stderr || ''}`;

if (result.status !== 0) {
  fail(`child exited ${result.status}\n${output}`);
}

if (!output.includes('Enter a number: ')) {
  fail(`expected question prompt in output\n${output}`);
}

if (!output.includes('42')) {
  fail(`expected piped input to reach readline\n${output}`);
}

if (!output.includes('You entered: 42')) {
  fail(`expected async continuation after question promise\n${output}`);
}

console.log('readline/promises question drains piped stdin before exit');
