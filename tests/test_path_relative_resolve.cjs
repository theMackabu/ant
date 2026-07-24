'use strict';

// path.relative must resolve both arguments against the working directory
// before diffing, matching Node's relative(resolve(from), resolve(to)).

const path = require('node:path');

function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}
function eq(name, got, want) {
  assert(got === want, `${name}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
}

eq("relative('.', 'm.js')", path.relative('.', 'm.js'), 'm.js');
eq("relative('', 'm.js')", path.relative('', 'm.js'), 'm.js');
eq("relative('.', '.')", path.relative('.', '.'), '');
eq("join+relative chain", path.relative(path.join('index.js', '..'), 'm.js'), 'm.js');
eq("relative('a', 'a/b/c.js')", path.relative('a', 'a/b/c.js'), 'b/c.js');
eq("relative('a/b', 'a')", path.relative('a/b', 'a'), '..');
eq("relative('./x', 'x/y')", path.relative('./x', 'x/y'), 'y');
eq("relative dot-dot inputs", path.relative('a/../b', 'c'), '../c');

const abs = path.resolve('m.js');
eq('relative(cwd, abs)', path.relative(process.cwd(), abs), 'm.js');
eq('relative(abs, abs)', path.relative(abs, abs), '');

console.log('path.relative resolve tests passed');
