const assert = require('node:assert');

const builders = {
  vars(n) { let s = 'var t = 0;'; for (let i = 0; i < n; i++) s += `var v${i} = ${i};`; s += `return v0 + v${n - 1} + v${n >> 1};`; return new Function(s)(); },
  lets(n) { let s = ''; for (let i = 0; i < n; i++) s += `let v${i} = ${i};`; s += `return v0 + v${n - 1} + v${n >> 1};`; return new Function(s)(); },
  consts(n) { let s = ''; for (let i = 0; i < n; i++) s += `const v${i} = ${i};`; s += `return v0 + v${n - 1} + v${n >> 1};`; return new Function(s)(); },
  varlist(n) { let s = 'var v0 = 0'; for (let i = 1; i < n; i++) s += `, v${i} = ${i}`; s += `; return v0 + v${n - 1} + v${n >> 1};`; return new Function(s)(); },
  funcs(n) { let s = ''; for (let i = 0; i < n; i++) s += `function f${i}() { return ${i} }`; s += `return f0() + f${n - 1}() + f${n >> 1}();`; return new Function(s)(); },
  upvaluesLet(n) { let s = ''; for (let i = 0; i < n; i++) s += `let v${i} = ${i};`; s += 'return () => { v0++; return 0'; for (let i = 0; i < n; i += 7) s += `+v${i}`; s += ' }'; return new Function(s)()(); },
  closuresWrite(n) { let s = ''; for (let i = 0; i < n; i++) s += `var v${i} = ${i};`; s += `var g = function () { v${n - 1} += 5; v0 = 3 }; g(); return [v0, v${n - 1}]`; return new Function(s)(); },
  tdz(n) { let s = 'var r = [];'; for (let i = 0; i < n; i++) s += `let v${i} = ${i};`; s += `try { z; } catch (e) { r.push(e.name) } let z = 1; const k = 2; try { k = 3 } catch (e) { r.push(e.name) } return r.concat(z, k, v${n - 1})`; return new Function(s)(); },
  loops(n) { let s = 'var fs = [];'; for (let i = 0; i < n; i++) s += `var v${i} = ${i};`; s += `for (let i = 0; i < 3; i++) { let w = i * 2; fs.push(() => i + w + v${n - 1}) } return fs.map(f => f())`; return new Function(s)(); },
  evalVars(n) { let s = ''; for (let i = 0; i < n; i++) s += `var v${i} = ${i};`; s += `return eval('v0 + v${n - 1}')`; return new Function(s)(); },
  argsObj(n) { let s = ''; for (let i = 0; i < n; i++) s += `var v${i} = ${i};`; s += `return arguments.length + v${n - 1}`; return new Function(s)(1, 2); },
  gen(n) { let s = 'function* g() {'; for (let i = 0; i < n; i++) s += `var v${i} = ${i};`; s += `yield v0; yield v${n - 1}; } return [...g()]`; return new Function(s)(); },
  destruct(n) { let s = 'var {'; for (let i = 0; i < n; i++) s += `v${i},`; s += `} = { v0: 1, v${n - 1}: 2 }; return [v0, v${n - 1}, v1 === undefined ? null : v1]`; return new Function(s)(); },
  catchv(n) { let s = ''; for (let i = 0; i < n; i++) s += `var v${i} = ${i};`; s += `try { throw 5 } catch (e) { var q = e + v${n - 1} } return q`; return new Function(s)(); },
  scriptLet(n) { let s = '{'; for (let i = 0; i < n; i++) s += `let sl${i} = ${i};`; s += `sl0 + sl${n - 1} }`; return (0, eval)(s); },
  hot(n) { let s = ''; for (let i = 0; i < n; i++) s += `var v${i} = ${i};`; s += `var t = 0; for (var i = 0; i < 20000; i++) { t += v${n - 1} + v3; v${n - 2} = t } return [t, v${n - 2}]`; return new Function(s)(); },
};

const expected = {"65535":{"vars":98301,"lets":98301,"consts":98301,"varlist":98301,"funcs":98301,"upvaluesLet":306797422,"closuresWrite":[3,65539],"tdz":["ReferenceError","TypeError",1,2,65534],"loops":[65534,65537,65540],"evalVars":65534,"argsObj":65536,"gen":[0,65534],"destruct":[1,2,null],"catchv":65539,"scriptLet":65534,"hot":[1310740000,1310740000]},"65536":{"vars":98303,"lets":98303,"consts":98303,"varlist":98303,"funcs":98303,"upvaluesLet":306797422,"closuresWrite":[3,65540],"tdz":["ReferenceError","TypeError",1,2,65535],"loops":[65535,65538,65541],"evalVars":65535,"argsObj":65537,"gen":[0,65535],"destruct":[1,2,null],"catchv":65540,"scriptLet":65535,"hot":[1310760000,1310760000]},"70000":{"vars":104999,"lets":104999,"consts":104999,"varlist":104999,"funcs":104999,"upvaluesLet":349965001,"closuresWrite":[3,70004],"tdz":["ReferenceError","TypeError",1,2,69999],"loops":[69999,70002,70005],"evalVars":69999,"argsObj":70001,"gen":[0,69999],"destruct":[1,2,null],"catchv":70004,"scriptLet":69999,"hot":[1400040000,1400040000]}};

for (const [count, results] of Object.entries(expected)) {
  for (const [name, want] of Object.entries(results)) {
    assert.deepStrictEqual(builders[name](+count), want, `${name} with ${count} bindings`);
  }
}

function manyCalls(n) {
  let s = '"use strict";';
  for (let i = 0; i < n; i++) s += `function f${i}() { return this === undefined ? ${i} : -1 }`;
  s += `return [f0(), f${n - 1}(), typeof f${n - 2}, delete f${n - 1}.name]`;
  return new Function(s)();
}
assert.deepStrictEqual(manyCalls(40000), [0, 39999, 'function', true]);

function shadowing(n) {
  let s = 'var outer = "o";';
  for (let i = 0; i < n; i++) s += `var v${i} = ${i};`;
  s += `{ let v${n - 1} = 'block'; var inner = () => v${n - 1} } var g = function (v${n - 2}) { return [v${n - 2}, v${n - 1}, outer] };`;
  s += `return [inner(), g('param'), v${n - 1}]`;
  return new Function(s)();
}
assert.deepStrictEqual(shadowing(40000), ['block', ['param', 39999, 'o'], 39999]);

console.log('ok');
