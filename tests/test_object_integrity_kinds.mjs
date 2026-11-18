import assert from 'node:assert';
import vm from 'node:vm';
function* gen() { yield 1 }
const kinds = {
  plain: () => ({ a: 1 }), empty: () => ({}), nullproto: () => Object.create(null), fn: () => function () {}, namedfn: () => function f(a, b) {}, arrow: () => () => {}, asyncfn: () => async function () {},
  genfn: () => function* () {}, klass: () => class {}, klass2: () => class A { static s = 1; f = 2 }, bound: () => (function () {}).bind(null), method: () => ({ m() {} }).m,
  argsMapped: () => (function (a) { return arguments })(1), argsUnmapped: () => (function (a) { 'use strict'; return arguments })(1), argsEmpty: () => (function () { return arguments })(),
  u8empty: () => new Uint8Array(0), u8: () => new Uint8Array(1), f64: () => new Float64Array(2), u8sub: () => new Uint8Array(new ArrayBuffer(8), 2, 0), buffer: () => Buffer.alloc(1), bufferEmpty: () => Buffer.alloc(0),
  arraybuffer: () => new ArrayBuffer(4), dataview: () => new DataView(new ArrayBuffer(4)), map: () => new Map([[1, 2]]), set: () => new Set([1]), weakmap: () => new WeakMap(), date: () => new Date(0),
  regexp: () => /a/g, error: () => new Error('e'), typeerror: () => new TypeError('t'), promise: () => Promise.resolve(1), genobj: () => gen(), proxy: () => new Proxy({ a: 1 }, {}), proxyfn: () => new Proxy(function () {}, {}),
  proxyarr: () => new Proxy([1], {}), boxedNum: () => new Number(1), boxedStr: () => new String('ab'), boxedStrEmpty: () => new String(''), boxedBool: () => new Boolean(true), boxedSym: () => Object(Symbol('s')), boxedBig: () => Object(1n),
  arr: () => [1, 2], arrEmpty: () => [], arrHoley: () => [1, , 3], arrBig: () => { const a = []; a[100] = 1; return a }, arrlike: () => ({ length: 1, 0: 'a' }), math: () => Object.create(Math), json: () => Object.create(JSON),
  symbol: () => Symbol('p'), num: () => 1, str: () => 's', undef: () => undefined, nul: () => null, bigint: () => 1n, bool: () => true,
  accessor: () => ({ get a() { return 1 }, set a(v) {} }), getterOnly: () => ({ get a() { return 1 } }), symKeyed: () => ({ [Symbol.iterator]: 1, a: 2 }), frozenAlready: () => Object.freeze({ a: 1 }), sealedAlready: () => Object.seal({ a: 1 }),
  nonExtensible: () => Object.preventExtensions({ a: 1 }), nonExtEmpty: () => Object.preventExtensions({}), definedRO: () => Object.defineProperty({}, 'a', { value: 1 }), definedNC: () => Object.defineProperty({}, 'a', { value: 1, writable: true }),
  ctxGlobal: () => vm.runInNewContext('this'), iter: () => [][Symbol.iterator](), mapIter: () => new Map().entries(), weakref: () => new WeakRef({}), sab: () => new SharedArrayBuffer(4), bigU64: () => new BigUint64Array(1),
  fnWithProps: () => Object.assign(function () {}, { a: 1 }), classInst: () => new (class { a = 1; #b = 2 })(), errWithCause: () => new Error('e', { cause: 1 }), url: () => new URL('http://a/b'), textenc: () => new TextEncoder(),
};
const ops = ['freeze', 'seal', 'preventExtensions'];
const queries = ['isFrozen', 'isSealed', 'isExtensible'];
const out = [];
for (const [name, make] of Object.entries(kinds)) {
  let base;
  try { const o = make(); base = queries.map(q => { try { return String(Object[q](o)) } catch (e) { return e.name } }).join(','); } catch (e) { base = 'MAKE ' + e.name; }
  const row = [name.padEnd(14), 'init=' + base];
  for (const op of ops) {
    let r;
    try {
      const o = make();
      let res;
      try { res = Object[op](o) === o ? 'same' : 'diff'; } catch (e) { res = e.name; }
      const q = queries.map(k => { try { return Object[k](o) ? 'T' : 'F' } catch (e) { return e.name } }).join('');
      let wr = '';
      if (o !== null && (typeof o === 'object' || typeof o === 'function')) {
        try { o.__newprop = 1; wr += ('__newprop' in o) ? 'A' : 'a'; } catch (e) { wr += 'x'; }
        try { const k = Reflect.ownKeys(o)[0]; if (k !== undefined) { const before = o[k]; const d = delete o[k]; wr += d && !(k in o) ? 'D' : 'd'; } else wr += '-'; } catch (e) { wr += 'x'; }
      }
      r = res + ':' + q + ':' + wr;
    } catch (e) { r = 'ERR ' + e.name; }
    row.push(op + '=' + r);
  }
  for (const rop of ['preventExtensions', 'isExtensible']) {
    try { const o = make(); row.push('R.' + rop + '=' + String(Reflect[rop](o))); } catch (e) { row.push('R.' + rop + '=' + e.name); }
  }
  out.push(row.join(' '));
}

const expected = [
  "plain          init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "empty          init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "nullproto      init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "fn             init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "namedfn        init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "arrow          init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "asyncfn        init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "genfn          init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "klass          init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "klass2         init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "bound          init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "method         init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "argsMapped     init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "argsUnmapped   init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "argsEmpty      init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "u8empty        init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "u8             init=false,false,true freeze=TypeError:FTF:xx seal=TypeError:FTF:xx preventExtensions=same:FTF:xx R.preventExtensions=true R.isExtensible=true",
  "f64            init=false,false,true freeze=TypeError:FTF:xx seal=TypeError:FTF:xx preventExtensions=same:FTF:xx R.preventExtensions=true R.isExtensible=true",
  "u8sub          init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "buffer         init=false,false,true freeze=TypeError:FTF:xx seal=TypeError:FTF:xx preventExtensions=same:FTF:xx R.preventExtensions=true R.isExtensible=true",
  "bufferEmpty    init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "arraybuffer    init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "dataview       init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "map            init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "set            init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "weakmap        init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "date           init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "regexp         init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FTF:xx R.preventExtensions=true R.isExtensible=true",
  "error          init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "typeerror      init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "promise        init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "genobj         init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "proxy          init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "proxyfn        init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "proxyarr       init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "boxedNum       init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "boxedStr       init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:TTF:xx R.preventExtensions=true R.isExtensible=true",
  "boxedStrEmpty  init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:TTF:xx R.preventExtensions=true R.isExtensible=true",
  "boxedBool      init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "boxedSym       init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "boxedBig       init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "arr            init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "arrEmpty       init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:TTF:xx R.preventExtensions=true R.isExtensible=true",
  "arrHoley       init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "arrBig         init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "arrlike        init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "math           init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "json           init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "symbol         init=true,true,false freeze=same:TTF: seal=same:TTF: preventExtensions=same:TTF: R.preventExtensions=TypeError R.isExtensible=TypeError",
  "num            init=true,true,false freeze=same:TTF: seal=same:TTF: preventExtensions=same:TTF: R.preventExtensions=TypeError R.isExtensible=TypeError",
  "str            init=true,true,false freeze=same:TTF: seal=same:TTF: preventExtensions=same:TTF: R.preventExtensions=TypeError R.isExtensible=TypeError",
  "undef          init=true,true,false freeze=same:TTF: seal=same:TTF: preventExtensions=same:TTF: R.preventExtensions=TypeError R.isExtensible=TypeError",
  "nul            init=true,true,false freeze=same:TTF: seal=same:TTF: preventExtensions=same:TTF: R.preventExtensions=TypeError R.isExtensible=TypeError",
  "bigint         init=true,true,false freeze=same:TTF: seal=same:TTF: preventExtensions=same:TTF: R.preventExtensions=TypeError R.isExtensible=TypeError",
  "bool           init=true,true,false freeze=same:TTF: seal=same:TTF: preventExtensions=same:TTF: R.preventExtensions=TypeError R.isExtensible=TypeError",
  "accessor       init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "getterOnly     init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "symKeyed       init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "frozenAlready  init=true,true,false freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:TTF:xx R.preventExtensions=true R.isExtensible=false",
  "sealedAlready  init=false,true,false freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FTF:xx R.preventExtensions=true R.isExtensible=false",
  "nonExtensible  init=false,false,false freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=false",
  "nonExtEmpty    init=true,true,false freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=false",
  "definedRO      init=false,false,true freeze=same:TTF:xx seal=same:TTF:xx preventExtensions=same:TTF:xx R.preventExtensions=true R.isExtensible=true",
  "definedNC      init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FTF:xx R.preventExtensions=true R.isExtensible=true",
  "ctxGlobal      init=false,false,true freeze=TypeError:FFT:AD seal=TypeError:FFT:AD preventExtensions=TypeError:FFT:AD R.preventExtensions=false R.isExtensible=true",
  "iter           init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "mapIter        init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "weakref        init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "sab            init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "bigU64         init=false,false,true freeze=TypeError:FTF:xx seal=TypeError:FTF:xx preventExtensions=same:FTF:xx R.preventExtensions=true R.isExtensible=true",
  "fnWithProps    init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xd R.preventExtensions=true R.isExtensible=true",
  "classInst      init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "errWithCause   init=false,false,true freeze=same:TTF:xx seal=same:FTF:xx preventExtensions=same:FFF:xD R.preventExtensions=true R.isExtensible=true",
  "url            init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
  "textenc        init=false,false,true freeze=same:TTF:x- seal=same:TTF:x- preventExtensions=same:TTF:x- R.preventExtensions=true R.isExtensible=true",
];
assert.deepStrictEqual(out.filter((row, i) => row !== expected[i]), []);
assert.strictEqual(out.length, expected.length);

for (const [source, message] of [['Object.freeze(this)', 'Cannot freeze'], ['Object.seal(this)', 'Cannot seal'], ['Object.preventExtensions(this)', 'Cannot prevent extensions']]) {
  assert.throws(() => vm.runInNewContext(source, {}), e => e.name === 'TypeError' && e.message === message);
}
assert.strictEqual(vm.runInNewContext('Reflect.preventExtensions(this)', {}), false);

console.log('ok');
