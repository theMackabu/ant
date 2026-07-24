// stores to properties named exec/replace must invalidate the regex fast
// paths even from JIT-compiled code: the put-field IC fastpath refuses
// those atoms so every such store reaches regexp_note_property_write

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

function putExec(o, f) { o.exec = f; }
function putReplace(o, f) { o.replace = f; }

// get both setters JIT-hot on plain objects first
for (let i = 0; i < 20000; i++) {
  const o = {};
  putExec(o, i);
  putReplace(o, i);
  if (o.exec !== i || o.replace !== i) throw new Error('warmup put failed');
}

// baseline: builtin exec through the literal fast path
assert(/needle/.exec('a needle b')[0] === 'needle', 'builtin exec baseline');
assert(/nope/.exec('a needle b') === null, 'builtin exec miss baseline');

// override RegExp.prototype.exec THROUGH THE JIT-COMPILED SETTER
const originalExec = RegExp.prototype.exec;
putExec(RegExp.prototype, function () { return ['overridden']; });

assert(/needle/.exec('a needle b')[0] === 'overridden',
  'literal exec honors JIT-written override');
assert(/nope/.exec('anything')[0] === 'overridden',
  'non-matching literal honors JIT-written override');

putExec(RegExp.prototype, originalExec);
assert(/needle/.exec('a needle b')[0] === 'needle', 'builtin exec restored');

// same for a replace override reached via String.prototype.replace
const originalReplace = RegExp.prototype[Symbol.replace];
putReplace(RegExp.prototype, undefined); // watched name, plain data prop
assert('a-b'.replace(/-/, '+') === 'a+b', 'replace still works after watched store');

console.log('jit regexp exec-override tests passed');
