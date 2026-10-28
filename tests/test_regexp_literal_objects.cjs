const out = [];
const mk = [() => /a/g, () => /a/dgimsy, () => /(?<x>a)(b)?/gi, () => /a/v, () => /\u{61}/u, () => /[/]/];
for (const f of mk) {
  const re = f();
  out.push(re.flags, re.source, String(re), re.global, re.sticky, re.unicodeSets, re.lastIndex, Object.prototype.toString.call(re));
  const c = structuredClone(re);
  out.push(c.flags, c.source, c instanceof RegExp, c !== re);
  out.push(JSON.stringify(re.exec('xab')), re.lastIndex);
  out.push(new RegExp(re).flags, new RegExp(re, 'y').flags, RegExp(re) === re);
}
const re = /a/g; re.compile('b', 'i'); out.push(re.flags, re.source, re.test('B'));
const re2 = /a/g; re2.foo = 1; out.push(Object.keys(re2).join(), re2.flags);
const re3 = /(?<y>\d{4})-(?<m>\d\d)/u; const m = re3.exec('2020-12'); out.push(m.groups.y, m.groups.m);
out.push(require('util').types.isRegExp(/x/), require('util').inspect(/x\/y/gi));
const keep = []; for (let i = 0; i < 200000; i++) { keep.push(/q+/g); if (keep.length > 1000) keep.length = 0; }
out.push(keep.length, keep[0] && keep[0].test('qq'));
out.push(Object.getOwnPropertyNames(/a/).join(), JSON.stringify(Object.getOwnPropertyDescriptor(/a/, 'lastIndex')));
const f = Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get; out.push(f.call(/a/gimsuy));
out.push(/a/g.flags === /b/g.flags, [/x/y].map(r => r.flags + r.sticky).join());
const expected = 'g|a|/a/g|true|false|false|0|[object RegExp]|g|a|true|true|["a"]|2|g|y|true|dgimsy|a|/a/dgimsy|true|true|false|0|[object RegExp]|dgimsy|a|true|true|null|0|dgimsy|y|true|gi|(?<x>a)(b)?|/(?<x>a)(b)?/gi|true|false|false|0|[object RegExp]|gi|(?<x>a)(b)?|true|true|["ab","a","b"]|3|gi|y|true|v|a|/a/v|false|false|true|0|[object RegExp]|v|a|true|true|["a"]|0|v|y|true|u|\\u{61}|/\\u{61}/u|false|false|false|0|[object RegExp]|u|\\u{61}|true|true|["a"]|0|u|y|true||[/]|/[/]/|false|false|false|0|[object RegExp]||[/]|true|true|null|0||y|true|i|b|true|foo|g|2020|12|true|/x\\/y/gi|801|true|lastIndex|{"value":0,"writable":true,"enumerable":false,"configurable":false}|gimsuy|true|ytrue';
if (out.join('|') !== expected) throw new Error('literal objects: expected ' + expected + ', got ' + out.join('|'));
console.log('ok');
