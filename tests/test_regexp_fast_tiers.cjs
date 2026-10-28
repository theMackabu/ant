function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

function battery(make) {
  const out = [];
  const s = 'x 12-34 y 5-6 é😀 z 7-8';
  const run = (name, fn) => {
    try { out.push([name, fn()]); } catch (e) { out.push([name, 'threw ' + e.name]); }
  };
  run('match g', () => { const re = make('(\\d+)-(\\d+)', 'g'); re.lastIndex = 3; return [s.match(re), re.lastIndex]; });
  run('match', () => { const re = make('(\\d+)-(\\d+)', ''); const m = s.match(re); return [m[0], m.index, m.length]; });
  run('match y', () => { const re = make('\\d', 'y'); re.lastIndex = 2; const m = s.match(re); return [m && m[0], re.lastIndex]; });
  run('replace str g', () => { const re = make('(\\d+)-(\\d+)', 'g'); re.lastIndex = 5; return [s.replace(re, '[$2|$1|$&|$`]'), re.lastIndex]; });
  run('replace str', () => { const re = make('(\\d+)', ''); return s.replace(re, '<$1>'); });
  run('replace sticky', () => { const re = make('\\d+', 'y'); re.lastIndex = 2; return [s.replace(re, '#'), re.lastIndex]; });
  run('replace sticky miss', () => { const re = make('\\d+', 'y'); re.lastIndex = 1; return [s.replace(re, '#'), re.lastIndex]; });
  run('replace fn', () => { const re = make('(?<a>\\d+)-(\\d+)', 'g'); return s.replace(re, (...a) => JSON.stringify(a.slice(0, 4)) + (typeof a[a.length - 1])); });
  run('replace named', () => { const re = make('(?<a>\\d+)-(?<b>\\d+)', 'g'); return s.replace(re, '$<b>-$<a>'); });
  run('replace empty gu', () => { const re = make('(?:)', 'gu'); return 'a😀b'.replace(re, '.'); });
  run('replace literal', () => { const re = make('é', 'g'); return s.replace(re, 'E'); });
  run('replace literal $', () => { const re = make('-', 'g'); return s.replace(re, '$&$&'); });
  run('replaceAll', () => { const re = make('\\d', 'g'); return s.replaceAll(re, '_'); });
  run('split', () => { const re = make('\\s+', ''); re.lastIndex = 4; return [s.split(re), re.lastIndex]; });
  run('split captures limit', () => { const re = make('(-)', ''); return s.split(re, 4); });
  run('split unicode empty', () => { const re = make('', 'u'); return 'a😀b'.split(re); });
  run('split nonascii', () => { const re = make('\\s', ''); return s.split(re); });
  run('search', () => { const re = make('\\d+', 'g'); re.lastIndex = 7; return [s.search(re), re.lastIndex]; });
  run('search literal', () => { const re = make('y', ''); return s.search(re); });
  run('search miss', () => { const re = make('q', ''); return s.search(re); });
  run('exec g', () => { const re = make('\\d+', 'g'); const r = []; let m; while ((m = re.exec(s))) r.push(m.index, re.lastIndex); return r; });
  run('exec d', () => { const re = make('(?<n>\\d+)', 'd'); const m = re.exec(s); return [m.indices[0], m.indices.groups.n]; });
  run('test', () => { const re = make('😀', 'g'); return [re.test(s), re.lastIndex, re.test(s), re.lastIndex]; });
  run('matchAll', () => { const re = make('\\d', 'g'); return [...s.matchAll(re)].map(m => m.index); });
  run('statics', () => { const re = make('(\\d)(\\d)', 'g'); s.replace(re, 'x'); return [RegExp.lastMatch, RegExp.$1, RegExp.$2, RegExp.lastParen, RegExp.leftContext.length]; });
  return out;
}
const expected = [["match g", [["12-34", "5-6", "7-8"], 0]], ["match", ["12-34", 2, 3]], ["match y", ["1", 3]], ["replace str g", ["x [34|12|12-34|x ] y [6|5|5-6|x 12-34 y ] \u00e9\ud83d\ude00 z [8|7|7-8|x 12-34 y 5-6 \u00e9\ud83d\ude00 z ]", 0]], ["replace str", "x <12>-34 y 5-6 \u00e9\ud83d\ude00 z 7-8"], ["replace sticky", ["x #-34 y 5-6 \u00e9\ud83d\ude00 z 7-8", 4]], ["replace sticky miss", ["x 12-34 y 5-6 \u00e9\ud83d\ude00 z 7-8", 0]], ["replace fn", "x [\"12-34\",\"12\",\"34\",2]object y [\"5-6\",\"5\",\"6\",10]object \u00e9\ud83d\ude00 z [\"7-8\",\"7\",\"8\",20]object"], ["replace named", "x 34-12 y 6-5 \u00e9\ud83d\ude00 z 8-7"], ["replace empty gu", ".a.\ud83d\ude00.b."], ["replace literal", "x 12-34 y 5-6 E\ud83d\ude00 z 7-8"], ["replace literal $", "x 12--34 y 5--6 \u00e9\ud83d\ude00 z 7--8"], ["replaceAll", "x __-__ y _-_ \u00e9\ud83d\ude00 z _-_"], ["split", [["x", "12-34", "y", "5-6", "\u00e9\ud83d\ude00", "z", "7-8"], 4]], ["split captures limit", ["x 12", "-", "34 y 5", "-"]], ["split unicode empty", ["a", "\ud83d\ude00", "b"]], ["split nonascii", ["x", "12-34", "y", "5-6", "\u00e9\ud83d\ude00", "z", "7-8"]], ["search", [2, 7]], ["search literal", 8], ["search miss", -1], ["exec g", [2, 4, 5, 7, 10, 11, 12, 13, 20, 21, 22, 23]], ["exec d", [[2, 4], [2, 4]]], ["test", [true, 17, false, 0]], ["matchAll", [2, 3, 5, 6, 10, 12, 20, 22]], ["statics", ["34", "3", "4", "4", 5]]];

function verify(tier, make) {
  const got = battery(make);
  check(`${tier} names`, got.map(e => e[0]), expected.map(e => e[0]));
  for (let i = 0; i < expected.length; i++) check(`${tier} ${expected[i][0]}`, got[i][1], expected[i][1]);
}

class Sub extends RegExp {}
const tiers = {
  pristine: (p, f) => new RegExp(p, f),
  own: (p, f) => { const re = new RegExp(p, f); re.extra = 1; return re; },
  sub: (p, f) => new Sub(p, f),
};
for (const [tier, make] of Object.entries(tiers)) verify(tier, make);
RegExp.prototype.extraMethod = function () {};
for (const [tier, make] of Object.entries(tiers)) verify(`invalidated ${tier}`, make);

console.log('ok');
