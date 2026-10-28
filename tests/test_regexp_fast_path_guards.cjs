const { spawnSync } = require('child_process');

function same(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ': ' + actual + ' !== ' + expected);
}

class Plain extends RegExp {}

const patterns = [
  ['\\s+', 'g'], ['(\\d+)-(\\d+)', ''], ['(\\d+)-(\\d+)', 'g'], ['(?<y>\\d+)-(?<m>\\d+)', 'g'],
  ['(?<y>\\d+)-(?<m>\\d+)', ''], ['lazy', 'g'], ['lazy', ''], ['o', 'gi'], ['', 'g'], ['', ''],
  ['x*', 'g'], ['(a)|(b)', 'g'], ['é', 'g'], ['b', 'y'], ['b', 'gy'], ['^\\w', 'gm'], ['(?:)', 'gu'],
  ['\\b', 'g'], ['[aeiou]', 'g'], ['(z)?q', 'g'], ['QBZPbageby', 'g'], ['\\bQBZPbageby\\b', ''],
];
const subjects = [
  'The quick brown fox 2020-01-02 jumps over the lazy dog 1999-12-31', '', 'abba', 'héllo wörld é',
  'bbb', 'a\nb\nc', 'qzqzq', '~'.repeat(200) + ' QBZPbageby ' + '~'.repeat(20),
];
const replacements = [
  '_', '$1', '[$&|$`|$\'|$$|$<m>|$<zz>|$2$1|$0|$10]', '', '$<y>',
  (...a) => JSON.stringify(a.map(v => typeof v === 'object' && v ? Object.entries(v) : v)),
];

function run(make, flags, s) {
  const r = [];
  for (const rep of replacements) { const rx = make(); r.push(s.replace(rx, rep), rx.lastIndex); }
  if (flags.includes('g')) for (const rep of replacements) { const rx = make(); r.push(s.replaceAll(rx, rep), rx.lastIndex); }
  { const rx = make(); r.push(JSON.stringify(s.match(rx)), rx.lastIndex); }
  { const rx = make(); rx.lastIndex = 3; r.push(JSON.stringify(s.match(rx)), rx.lastIndex); }
  { const rx = make(); r.push(s.search(rx), rx.lastIndex); }
  { const rx = make(); rx.lastIndex = 2; r.push(s.search(rx), rx.lastIndex); }
  { const rx = make(); r.push(JSON.stringify(s.split(rx)), JSON.stringify(s.split(rx, 2)), rx.lastIndex); }
  { const rx = make(); r.push(rx.test(s), rx.lastIndex, rx.test(s), rx.lastIndex); }
  { const rx = make(); const m = rx.exec(s); r.push(JSON.stringify(m), m && m.index, m && JSON.stringify(m.groups), rx.lastIndex); }
  if (flags.includes('g')) { const rx = make(); r.push(JSON.stringify([...s.matchAll(rx)].map(m => [m[0], m.index, m.groups]))); }
  { const rx = make(); rx.lastIndex = 5; r.push(s.replace(rx, '#'), rx.lastIndex); }
  return JSON.stringify(r);
}

for (const [pattern, flags] of patterns) {
  for (const subject of subjects) {
    same(
      run(() => new RegExp(pattern, flags), flags, subject),
      run(() => new Plain(pattern, flags), flags, subject),
      'fast and generic paths agree for /' + pattern + '/' + flags + ' on ' + JSON.stringify(subject)
    );
  }
}

const date = '2020-01-02 1999-12-31';
same(date.replace(/(?<y>\d{4})-(?<m>\d\d)(?<z>x)?/g, '$<m>/$<y>$<z>$<nope>|'), '01/2020|-02 12/1999|-31', 'named substitution');
same(date.replace(/(\d+)/, '$<y>'), '$<y>-01-02 1999-12-31', 'named substitution without groups');
same(date.replace(/(?<y>\d{4})/, (...a) => JSON.stringify(a[a.length - 1])), '{"y":"2020"}-01-02 1999-12-31', 'named groups argument');
same(JSON.stringify(Object.keys(/(?<y>a)(?<m>b)/.exec('ab').groups)), '["y","m"]', 'groups keep pattern order');

const prelude = `
const S = 'a1b22c333 d4444 e';
function battery() {
  const g = /(\\d)(\\d)?/g, one = /(\\d)(\\d)?/;
  const e = /\\d+/g;
  return JSON.stringify([
    S.replace(g, '[$2$1]'), g.lastIndex, S.replace(one, '<$&>'), one.lastIndex,
    S.replace(/\\d+/g, (m, i) => m.length + '@' + i), S.replaceAll(/\\d+/g, '_'),
    S.match(/\\d+/g), S.match(/(\\d)\\d/), [...S.matchAll(/\\d+/g)].map(m => m[0] + m.index),
    S.split(/(\\d)\\d*/), S.search(/\\s/), /c/.test(S), e.exec(S), e.lastIndex,
  ]);
}
let warm = 0;
for (let i = 0; i < 2000; i++) warm += battery().length;
const before = battery();
const out = [];
`;

const scenarios = [
  ['prototype exec assignment', `
    const orig = RegExp.prototype.exec; let calls = 0;
    RegExp.prototype.exec = function (s) { calls++; return orig.call(this, s); };
    out.push(battery() === before, calls);`, [true, 43]],
  ['prototype exec defineProperty', `
    const orig = RegExp.prototype.exec; let calls = 0;
    Object.defineProperty(RegExp.prototype, 'exec', { value: function (s) { calls++; return orig.call(this, s); } });
    out.push(battery() === before, calls);`, [true, 43]],
  ['prototype exec store from a warm site', `
    const orig = RegExp.prototype.exec; let calls = 0;
    function put(o, f) { o.exec = f; }
    for (let i = 0; i < 5000; i++) put({ exec: 0, other: i }, i);
    put(RegExp.prototype, function (s) { calls++; return orig.call(this, s); });
    out.push(battery() === before, calls);`, [true, 43]],
  ['symbol store from a warm keyed site', `
    function put(o, k, v) { o[k] = v; }
    for (let i = 0; i < 5000; i++) put({ [Symbol.replace]: 0, [Symbol.split]: 0 }, i & 1 ? Symbol.replace : Symbol.split, i);
    put(RegExp.prototype, Symbol.replace, function (s) { return 'R:' + s; });
    put(RegExp.prototype, Symbol.split, function (s) { return ['S', s]; });
    out.push(S.replace(/\d/g, '#'), JSON.stringify(S.split(/\d+/)));`, ['R:a1b22c333 d4444 e', '["S","a1b22c333 d4444 e"]']],
  ['deleted Symbol.replace', `
    delete RegExp.prototype[Symbol.replace];
    out.push('x/a/g and a'.replace(/a/g, 'X'));`, ['xX and a']],
  ['replaced symbol methods', `
    RegExp.prototype[Symbol.split] = function (s, l) { return ['split', s, l]; };
    RegExp.prototype[Symbol.match] = function (s) { return ['match', s]; };
    RegExp.prototype[Symbol.search] = function (s) { return 'search:' + s; };
    RegExp.prototype[Symbol.replace] = function (s, r) { return 'replace:' + s + ':' + r; };
    out.push(JSON.stringify(['a,b'.split(/,/, 3), 'ab'.match(/b/), 'ab'.search(/b/), 'ab'.replace(/b/g, 'c')]));`,
    ['[["split","a,b",3],["match","ab"],"search:ab","replace:ab:c"]']],
  ['Symbol.species', `
    let calls = 0;
    Object.defineProperty(RegExp, Symbol.species, { configurable: true, get() { return function (src, flags) { calls++; return new RegExp(src.source || src, flags); }; } });
    out.push(JSON.stringify(S.split(/\\d+/)), calls);`, ['["a","b","c"," d"," e"]', 1]],
  ['prototype constructor', `
    let calls = 0;
    function Fake(src, flags) { calls++; return new RegExp(src.source || src, flags); }
    Fake[Symbol.species] = Fake;
    RegExp.prototype.constructor = Fake;
    out.push(JSON.stringify(S.split(/\\d+/)), calls);`, ['["a","b","c"," d"," e"]', 1]],
  ['own exec', `
    const rx = /\\d+/g; let calls = 0;
    rx.exec = function (s) { calls++; return RegExp.prototype.exec.call(this, s); };
    out.push(S.replace(rx, '#'), S.search(rx), rx.test(S), calls, battery() === before);`, ['a#b#c# d# e', 1, true, 7, true]],
  ['lastIndex accessor', `
    const rx = /\\d/g;
    try { Object.defineProperty(rx, 'lastIndex', { get() { return 0; } }); out.push('defined'); } catch (e) { out.push(e.constructor.name); }
    out.push(S.replace(rx, '#'));`, ['TypeError', 'a#b##c### d#### e']],
  ['read-only lastIndex', `
    const rx = /\\d/g; Object.defineProperty(rx, 'lastIndex', { writable: false });
    const res = [];
    for (const f of [() => S.replace(rx, '#'), () => S.match(rx), () => S.replace(rx, () => 'z'), () => rx.exec(S)]) {
      try { res.push(JSON.stringify(f())); } catch (e) { res.push(e.constructor.name); }
    }
    try { res.push(S.replace(Object.freeze(/\\d/g), '#')); } catch (e) { res.push(e.constructor.name); }
    out.push(res.join(), battery() === before);`, ['TypeError,TypeError,TypeError,TypeError,TypeError', true]],
  ['object lastIndex', `
    const rx = /\\d/y; let calls = 0;
    rx.lastIndex = { valueOf() { calls++; return 1; } };
    out.push(S.replace(rx, '#'), rx.lastIndex, calls);`, ['a#b22c333 d4444 e', 2, 1]],
  ['prototype swapped on instance', `
    let calls = 0;
    class Sub extends RegExp { exec(s) { calls++; return super.exec(s); } }
    const rx = /\\d+/g; Object.setPrototypeOf(rx, Sub.prototype);
    out.push(S.replace(rx, '#'), calls, battery() === before);`, ['a#b#c# d# e', 5, true]],
  ['subclass exec and species', `
    let calls = 0, made = 0;
    class Sub extends RegExp {
      exec(s) { calls++; return super.exec(s); }
      static get [Symbol.species]() { made++; return Sub; }
    }
    const rx = new Sub('(\\\\d)(\\\\d)?', 'g');
    out.push(S.replace(rx, '[$2$1]'), JSON.stringify(S.split(rx)), calls, made);`,
    ['a[1]b[22]c[33][3] d[44][44] e', '["a","1",null,"b","2","2","c","3","3","","3",null," d","4","4","","4","4"," e"]', 20, 1]],
  ['proxy receiver', `
    const keys = [];
    const p = new Proxy(/\\d/g, { get(t, k) { keys.push(typeof k === 'symbol' ? k.description : k); const v = Reflect.get(t, k); return typeof v === 'function' ? v.bind(t) : v; } });
    out.push(S.replace(p, '#'), keys.join());`, ['a#b##c### d#### e', 'Symbol.replace']],
  ['replacement toString mutates the prototype', `
    const orig = RegExp.prototype.exec; let calls = 0;
    const rep = { toString() { RegExp.prototype.exec = function (s) { calls++; return orig.call(this, s); }; return '#'; } };
    out.push(S.replace(/\\d+/g, rep), calls);`, ['a#b#c# d# e', 5]],
  ['replacer mutates state', `
    const rx = /\\d+/g; const orig = RegExp.prototype.exec; let calls = 0;
    const res = S.replace(rx, (m) => {
      rx.lastIndex = 7;
      RegExp.prototype.exec = function (s) { calls++; return orig.call(this, s); };
      return '<' + m + '>';
    });
    out.push(res, rx.lastIndex, calls, RegExp.lastMatch);`, ['a<1>b<22>c<333> d<4444> e', 7, 0, '4444']],
  ['exec after a warm loop', `
    const re = /\\d+/; let calls = 0, hits = 0;
    function loop(n) { for (let i = 0; i < n; i++) { re.exec(S); if (re.exec(S)) hits++; } }
    loop(5000);
    const orig = RegExp.prototype.exec;
    RegExp.prototype.exec = function (s) { calls++; return orig.call(this, s); };
    loop(10);
    out.push(calls, hits);`, [20, 5010]],
  ['own Symbol.replace', `
    const rx = /b/g; Object.defineProperty(rx, Symbol.replace, { value: function (s, r) { return 'own:' + s + r; } });
    out.push('abc'.replace(rx, '!'), 'abc'.replace(/b/g, '!'), battery() === before);`, ['own:abc!', 'a!c', true]],
  ['groups from a custom exec', `
    class G extends RegExp { exec(str) { const r = super.exec(str); if (r) r.groups = { m: 'M', y: { toString() { return 'Y'; } } }; return r; } }
    out.push('1 2'.replace(new G('\\\\d', 'g'), '$<m>$<y>$<q>'), '1 2'.replace(new G('\\\\d'), (...a) => typeof a[a.length - 1]));`,
    ['MY MY', 'object 2']],
];

for (const [name, body, expected] of scenarios) {
  const code = prelude + body + '\nconsole.log(JSON.stringify(out));';
  const child = spawnSync(process.execPath, ['-e', code], { encoding: 'utf8', timeout: 60000 });
  same(child.status, 0, name + ' exit status (' + child.stderr + ')');
  same(child.stdout.trim(), JSON.stringify(expected), name);
}

console.log('ok');
