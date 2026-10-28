const desc = Object.getOwnPropertyDescriptor(RegExp.prototype, 'source');
const fdesc = Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags');
let srcCalls = 0;
Object.defineProperty(RegExp.prototype, 'source', { get() { srcCalls++; return 'q'; }, configurable: true });
const out = [];
out.push('xbxb'.split(/b/).length, [...'xbxb'.matchAll(/b/g)].length);
out.push(new RegExp(/b/g).test('b'), RegExp(/b/g, 'i').test('B'), new RegExp(/b/).flags, new RegExp(/b/gi).flags);
out.push(/z/.compile(/b/g).test('b'));
try { /z/.compile(/b/g, 'i'); out.push('no throw'); } catch (e) { out.push(e.name); }
const re = /x/g; re.lastIndex = 2;
const it = re[Symbol.matchAll]('xxxx'); out.push([...it].map(m => m.index).join());
out.push(srcCalls);
Object.defineProperty(RegExp.prototype, 'source', desc);
class R extends RegExp { static get [Symbol.species]() { return RegExp; } }
const r = new R('a', 'g'); out.push([...'aa'.matchAll(r)].length, r.constructor.name);
let speciesCalls = 0;
class S extends RegExp { constructor(...a) { speciesCalls++; super(...a); } }
const s = new S('a', 'g'); [...'aa'.matchAll(s)]; 'a,a'.split(new S(','));
out.push(speciesCalls);
const big = /a/g; big.lastIndex = { valueOf() { return 1; } };
out.push([...'aaa'.matchAll(big)].map(m => m.index).join());
try { const b = /a/g; b.lastIndex = 1n; [...'aa'.matchAll(b)]; out.push('no throw'); } catch (e) { out.push(e.name); }
const expected = '[3,2,true,true,"","gi",true,"TypeError","2,3",0,2,"R",4,"1,2","TypeError"]';
if (JSON.stringify(out) !== expected) throw new Error('original source: expected ' + expected + ', got ' + JSON.stringify(out));
console.log('ok');
