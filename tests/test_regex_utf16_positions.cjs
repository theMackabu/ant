'use strict';

// Regex positions visible to JS (match.index, lastIndex, d-flag indices,
// replacer offsets, search, split boundaries) must be UTF-16 code units,
// not byte offsets into the runtime's UTF-8 string storage.

function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  assert(g === w, `${name}: got ${g}, want ${w}`);
}

const s = 'é'.repeat(100) + 'MARKER_END';
eq('exec index', /MARKER_END/.exec(s).index, 100);
eq('matchAll index', [...s.matchAll(/MARKER_END/g)][0].index, 100);
eq('search', s.search(/MARKER_END/), 100);
eq('indexOf agreement', s.indexOf('MARKER_END'), /MARKER_END/.exec(s).index);

const g = /X/g;
const t = 'ééXééX';
const m1 = g.exec(t);
eq('global exec index', m1.index, 2);
eq('lastIndex after match', g.lastIndex, 3);
const m2 = g.exec(t);
eq('second exec index', m2.index, 5);
eq('lastIndex round-trip', g.lastIndex, 6);
eq('exhausted', g.exec(t), null);

const d = /(X)(Y)?/dg.exec('ééXZ');
eq('d-flag indices', d.indices[0], [2, 3]);
eq('d-flag capture indices', d.indices[1], [2, 3]);

eq('replace fn offset', 'ééaééa'.replace(/a/g, (m, off) => String(off)), 'éé2éé5');
eq('replace before-match', 'ééaX'.replace(/X/, '[$`]'), 'ééa[ééa]');
eq('replace after-match', 'Xaéé'.replace(/X/, "[$']"), '[aéé]aéé');
eq('split multibyte separator', 'aéébééc'.split(/éé/), ['a', 'b', 'c']);
eq('split keeps captures', 'aéXéb'.split(/é(X)é/), ['a', 'X', 'b']);

const y = /X/y;
y.lastIndex = 2;
eq('sticky lastIndex in units', y.test('ééXé'), true);

eq('surrogate pair indices', [...'😀a😀a'.matchAll(/a/g)].map((m) => m.index), [2, 5]);
eq('unicode empty-match advance', [...'😀b'.matchAll(/(?:)/gu)].length, 3);
eq('string match global', 'ééXééX'.match(/X/g), ['X', 'X']);

// lastIndex beyond utf16 length resets and misses
const z = /M/g;
z.lastIndex = 200;
eq('lastIndex past end', z.exec('ééM'), null);
eq('lastIndex reset after miss', z.lastIndex, 0);

// WTF-8 subjects (lone surrogates are invalid UTF-8): a jit match must
// not arm the validation cache, and interpreted matches must stay safe
{
  const wtf = 'a' + String.fromCharCode(0xd800) + 'bXc';
  const g = /X/g;
  eq('wtf8 global exec', g.exec(wtf) && g.exec(wtf.slice(0)), null);
  const y = /b/y;
  y.lastIndex = 2;
  eq('wtf8 sticky after global', y.test(wtf), true);
}

// empty-match loops over non-ASCII must advance by whole characters
eq('empty-match match loop unicode', 'é'.match(/x*/g), ['', '']);
// Node yields three empties (it can sit mid-surrogate-pair, positions
// 0/1/2); WTF-8 storage has no mid-pair byte position, so the engine's
// whole-character convention yields two
eq('empty-match match loop astral', '😀'.match(/x*/g), ['', '']);
eq('non-u empty split astral', '😀a'.split(/(?:)/).length >= 2, true);

console.log('regex utf16 position tests passed');
