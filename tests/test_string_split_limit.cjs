const out = [];
const lims = [undefined, NaN, 0, -0, 0.5, 1, 1.9, -1, -1.5, 2 ** 32, 2 ** 32 + 1, 2 ** 32 + 2, -(2 ** 32) + 1, Infinity, -Infinity, '2', '1e0', 'x', null, true, false, [], [2], { valueOf() { return 1; } }];
for (const l of lims) {
  out.push([
    'a,b,c'.split(',', l).length, 'a,b,c'.split(/,/, l).length, 'a,b,c'.split(/,/y, l).length,
    'abc'.split('', l).length, 'a,b,c'.split(/(,)/, l).length, 'é,b,c'.split(/,/, l).length,
    /,/[Symbol.split]('a,b,c', l).length,
  ].join(''));
}
const log = [];
const sep = { [Symbol.split]: undefined, toString() { log.push('sep'); return ','; } };
'a,b'.split(sep, { valueOf() { log.push('lim'); return 5; } });
out.push(log.join());
try { 'a,b'.split(',', 1n); out.push('no'); } catch (e) { out.push(e.name); }
try { 'a,b'.split(/,/, 1n); out.push('no'); } catch (e) { out.push(e.name); }
try { 'a,b'.split(',', Symbol()); out.push('no'); } catch (e) { out.push(e.name); }
try { 'a,b'.split(/,/, { valueOf() { throw new RangeError('x'); } }); out.push('no'); } catch (e) { out.push(e.name); }
const expected = '3333533|0000000|0000000|0000000|0000000|1111111|1111111|3333533|3333533|0000000|1111111|2222222|1111111|0000000|0000000|2222222|1111111|0000000|0000000|1111111|0000000|0000000|2222222|1111111|lim,sep|TypeError|TypeError|TypeError|RangeError';
if (out.join('|') !== expected) throw new Error('split limit: expected ' + expected + ', got ' + out.join('|'));
console.log('ok');
