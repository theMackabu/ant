const { spawnSync } = require('child_process');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function run(source) {
  const result = spawnSync(process.execPath, ['-e', source], { encoding: 'utf8' });
  if (result.error) throw result.error;
  assert(
    result.status === 0,
    `expected console.inspect probe to exit 0, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  );
  return result.stdout;
}

const flat = run('console.inspect("hello world")');
assert(flat.includes('<String flat '), `expected flat string internals, got ${JSON.stringify(flat)}`);
assert(/value=0x[0-9a-f]+/.test(flat), `expected raw value bits, got ${JSON.stringify(flat)}`);
assert(/data=0x[0-9a-f]+/.test(flat), `expected raw data bits, got ${JSON.stringify(flat)}`);
assert(/ptr=0x[0-9a-f]+/.test(flat), `expected flat string pointer, got ${JSON.stringify(flat)}`);
assert(flat.includes('len=11'), `expected flat string length, got ${JSON.stringify(flat)}`);
assert(flat.includes('ascii=yes'), `expected ASCII state, got ${JSON.stringify(flat)}`);
assert(flat.includes('bytes="hello world"'), `expected byte preview, got ${JSON.stringify(flat)}`);

const rope = run('console.inspect("a" + "b")');
assert(rope.includes('<String rope '), `expected rope string internals, got ${JSON.stringify(rope)}`);
assert(rope.includes('depth=1'), `expected rope depth, got ${JSON.stringify(rope)}`);
assert(rope.includes('cached=undefined'), `expected untouched rope cache, got ${JSON.stringify(rope)}`);
assert(rope.includes('left: <String flat '), `expected left flat leaf, got ${JSON.stringify(rope)}`);
assert(rope.includes('right: <String flat '), `expected right flat leaf, got ${JSON.stringify(rope)}`);

console.log('console.inspect string internals ok');
