class Sub extends RegExp {}

function both(pattern, flags, fn) {
  return [fn(new RegExp(pattern, flags)), fn(new Sub(pattern, flags))];
}

function execLoop(rx, s) {
  const out = [];
  for (let i = 0; i < 50; i++) {
    const m = rx.exec(s);
    if (!m) { out.push(null, rx.lastIndex); break; }
    out.push(m.index, m[0], rx.lastIndex);
    if (m[0] === '') rx.lastIndex++;
  }
  return out;
}

function fromLastIndex(rx, s, li) {
  rx.lastIndex = li;
  const m = rx.exec(s);
  return [m && m.index, m && m[0], rx.lastIndex];
}

function positions(...a) {
  const offset = typeof a[a.length - 1] === 'object' ? a[a.length - 3] : a[a.length - 2];
  return '<' + a[0] + '@' + offset + '>';
}

function testChars(pattern, flags, chars) {
  const rx = new RegExp(pattern, flags);
  return chars.map(c => (rx.test(c) ? 1 : 0)).join('');
}

const BMP = 'héllo wörld é';
const ASTRAL = 'a😀b😀😀c';
const LONE = 'a\ud800b\udc00c';
const CHARS = ['é', 'ß', '٣', '３', '\u00a0', '\ufeff', '\u2028', '\u2029', '\u180e', '\u0085', '\u3000', 'ſ', 'K', 'k', 's', 'S', '_', '7', 'a', ' '];

let failures = 0;
function check(label, actual, expected) {
  const got = JSON.stringify(actual);
  if (got !== expected) {
    failures++;
    console.log('FAIL ' + label + '\n  expected ' + expected + '\n  actual   ' + got);
  }
}

check("test lastIndex BMP", both('o', 'g', r => [r.test('héllo'), r.lastIndex, r.test('héllo'), r.lastIndex]), "[[true,5,false,0],[true,5,false,0]]");
check("exec lastIndex BMP", both('l+', 'g', r => { const m = r.exec(BMP); return [m.index, m[0], r.lastIndex]; }), "[[2,\"ll\",4],[2,\"ll\",4]]");
check("exec loop BMP", both('[éö]|l', 'g', r => execLoop(r, BMP)), "[[1,\"é\",2,2,\"l\",3,3,\"l\",4,7,\"ö\",8,9,\"l\",10,12,\"é\",13,null,0],[1,\"é\",2,2,\"l\",3,3,\"l\",4,7,\"ö\",8,9,\"l\",10,12,\"é\",13,null,0]]");
check("exec loop BMP sticky", both('h|é|l', 'y', r => execLoop(r, BMP)), "[[0,\"h\",1,1,\"é\",2,2,\"l\",3,3,\"l\",4,null,0],[0,\"h\",1,1,\"é\",2,2,\"l\",3,3,\"l\",4,null,0]]");
check("exec loop astral u", both('😀|b', 'gu', r => execLoop(r, ASTRAL)), "[[1,\"😀\",3,3,\"b\",4,4,\"😀\",6,6,\"😀\",8,null,0],[1,\"😀\",3,3,\"b\",4,4,\"😀\",6,6,\"😀\",8,null,0]]");
check("exec loop astral v", both('[😀c]', 'gv', r => execLoop(r, ASTRAL)), "[[1,\"😀\",3,4,\"😀\",6,6,\"😀\",8,8,\"c\",9,null,0],[1,\"😀\",3,4,\"😀\",6,6,\"😀\",8,8,\"c\",9,null,0]]");
check("exec loop astral literal", both('b|c', 'g', r => execLoop(r, ASTRAL)), "[[3,\"b\",4,8,\"c\",9,null,0],[3,\"b\",4,8,\"c\",9,null,0]]");
check("exec loop empty u", both('(?:)', 'gu', r => execLoop(r, ASTRAL)), "[[0,\"\",0,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1],[0,\"\",0,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1,1,\"\",1]]");
check("exec loop empty BMP", both('(?:)', 'g', r => execLoop(r, 'aé😀'.slice(0, 2) + 'x')), "[[0,\"\",0,1,\"\",1,2,\"\",2,3,\"\",3,null,0],[0,\"\",0,1,\"\",1,2,\"\",2,3,\"\",3,null,0]]");
check("exec loop lone surrogates", both('[bc]', 'g', r => execLoop(r, LONE)), "[[2,\"b\",3,4,\"c\",5,null,0],[2,\"b\",3,4,\"c\",5,null,0]]");
check("exec loop lone surrogates u", both('.', 'gu', r => execLoop(r, LONE)), "[[0,\"a\",1,1,\"\\ud800\",2,2,\"b\",3,3,\"\\udc00\",4,4,\"c\",5,null,0],[0,\"a\",1,1,\"\\ud800\",2,2,\"b\",3,3,\"\\udc00\",4,4,\"c\",5,null,0]]");
check("lastIndex past end", both('a', 'g', r => fromLastIndex(r, 'éa', 3)), "[[null,null,0],[null,null,0]]");
check("lastIndex at end", both('$', 'g', r => fromLastIndex(r, 'éa', 2)), "[[2,\"\",2],[2,\"\",2]]");
check("lastIndex inside BMP text", both('a', 'g', r => fromLastIndex(r, 'éa', 1)), "[[1,\"a\",2],[1,\"a\",2]]");
check("lastIndex mid pair u", both('.', 'gu', r => fromLastIndex(r, '😀a', 1)), "[[0,\"😀\",2],[0,\"😀\",2]]");
check("lastIndex mid pair u empty", both('(?:)', 'gu', r => fromLastIndex(r, '😀a', 1)), "[[0,\"\",0],[0,\"\",0]]");
check("lastIndex mid pair u sticky", both('.', 'yu', r => fromLastIndex(r, '😀a', 1)), "[[0,\"😀\",2],[0,\"😀\",2]]");
check("lastIndex mid pair u sticky miss", both('a', 'yu', r => fromLastIndex(r, '😀a', 1)), "[[null,null,0],[null,null,0]]");
check("lastIndex mid pair v", both('\\p{Emoji_Presentation}', 'gv', r => fromLastIndex(r, 'x😀😀', 2)), "[[1,\"😀\",3],[1,\"😀\",3]]");
check("lastIndex mid pair non-u", both('a', 'g', r => fromLastIndex(r, '😀a', 1)), "[[2,\"a\",3],[2,\"a\",3]]");
check("lastIndex mid pair end u", both('.', 'gu', r => fromLastIndex(r, 'a😀', 2)), "[[1,\"😀\",3],[1,\"😀\",3]]");
check("lastIndex every position u", both('[😀é]', 'gu', r => [0, 1, 2, 3, 4, 5, 6].map(i => fromLastIndex(r, 'é😀😀', i))), "[[[0,\"é\",1],[1,\"😀\",3],[1,\"😀\",3],[3,\"😀\",5],[3,\"😀\",5],[null,null,0],[null,null,0]],[[0,\"é\",1],[1,\"😀\",3],[1,\"😀\",3],[3,\"😀\",5],[3,\"😀\",5],[null,null,0],[null,null,0]]]");
check("lastIndex every position BMP", both('[éö]', 'g', r => [0, 1, 2, 3, 4, 5, 6, 7, 8].map(i => fromLastIndex(r, BMP, i))), "[[[1,\"é\",2],[1,\"é\",2],[7,\"ö\",8],[7,\"ö\",8],[7,\"ö\",8],[7,\"ö\",8],[7,\"ö\",8],[7,\"ö\",8],[12,\"é\",13]],[[1,\"é\",2],[1,\"é\",2],[7,\"ö\",8],[7,\"ö\",8],[7,\"ö\",8],[7,\"ö\",8],[7,\"ö\",8],[7,\"ö\",8],[12,\"é\",13]]]");
check("lastIndex fractional", both('l', 'g', r => fromLastIndex(r, 'héllo', 2.7)), "[[2,\"l\",3],[2,\"l\",3]]");
check("sticky test BMP", both('l', 'y', r => [2, 3, 4].map(i => { r.lastIndex = i; return [r.test('héllo'), r.lastIndex]; })), "[[[true,3],[true,4],[false,0]],[[true,3],[true,4],[false,0]]]");
check("index d flag", both('(é)(?<x>l+)', 'd', r => { const m = r.exec('héllo'); return [m.index, m.indices, m.indices.groups.x]; }), "[[1,[[1,4],[1,2],[2,4]],[2,4]],[1,[[1,4],[1,2],[2,4]],[2,4]]]");
check("index d flag astral", both('(😀)(?<x>l+)', 'du', r => { const m = r.exec('a😀llo'); return [m.index, m.indices, m.indices.groups.x]; }), "[[1,[[1,5],[1,3],[3,5]],[3,5]],[1,[[1,5],[1,3],[3,5]],[3,5]]]");
check("index d flag lookbehind", both('(?<=(é))l', 'dg', r => { r.lastIndex = 2; const m = r.exec('héllo'); return [m.index, m.indices, r.lastIndex]; }), "[[2,[[2,3],[1,2]],3],[2,[[2,3],[1,2]],3]]");
check("index d flag unmatched", both('(x)?(ö)', 'd', r => { const m = r.exec('wörld'); return [m.indices]; }), "[[[[1,2],null,[1,2]]],[[[1,2],null,[1,2]]]]");
check("matchAll d flag", both('[éö]', 'gd', r => [...BMP.matchAll(r)].map(m => [m.index, m.indices[0]])), "[[[1,[1,2]],[7,[7,8]],[12,[12,13]]],[[1,[1,2]],[7,[7,8]],[12,[12,13]]]]");
check("match global", both('[éö]', 'g', r => [BMP.match(r), r.lastIndex]), "[[[\"é\",\"ö\",\"é\"],0],[[\"é\",\"ö\",\"é\"],0]]");
check("match non-global", both('ö', '', r => { const m = BMP.match(r); return [m.index, r.lastIndex]; }), "[[7,0],[7,0]]");
check("match sticky", both('ö', 'y', r => { r.lastIndex = 7; const m = BMP.match(r); return [m && m.index, r.lastIndex]; }), "[[7,8],[7,8]]");
check("match empty u", both('(?:)', 'gu', r => ASTRAL.match(r).length), "[7,7]");
check("matchAll", both('l|😀', 'gu', r => [...'é😀l😀ll'.matchAll(r)].map(m => [m.index, m[0]])), "[[[1,\"😀\"],[3,\"l\"],[4,\"😀\"],[6,\"l\"],[7,\"l\"]],[[1,\"😀\"],[3,\"l\"],[4,\"😀\"],[6,\"l\"],[7,\"l\"]]]");
check("matchAll empty u", both('(?:)', 'gu', r => [...'é😀x'.matchAll(r)].map(m => m.index)), "[[0,1,3,4],[0,1,3,4]]");
check("matchAll empty BMP", both('\\b', 'g', r => [...BMP.matchAll(r)].map(m => m.index)), "[[0,1,2,5,6,7,8,11],[0,1,2,5,6,7,8,11]]");
check("matchAll lastIndex", both('l', 'g', r => { r.lastIndex = 3; return [...'héllo l'.matchAll(r)].map(m => m.index); }), "[[3,6],[3,6]]");
check("replace string", both('l', 'g', r => [BMP.replace(r, '[$`|$&]'), r.lastIndex]), "[[\"hé[hé|l][hél|l]o wör[héllo wör|l]d é\",0],[\"hé[hé|l][hél|l]o wör[héllo wör|l]d é\",0]]");
check("replace function positions", both('[éöl]', 'g', r => BMP.replace(r, positions)), "[\"h<é@1><l@2><l@3>o w<ö@7>r<l@9>d <é@12>\",\"h<é@1><l@2><l@3>o w<ö@7>r<l@9>d <é@12>\"]");
check("replace function positions astral", both('[😀c]', 'gu', r => ASTRAL.replace(r, positions)), "[\"a<😀@1>b<😀@4><😀@6><c@8>\",\"a<😀@1>b<😀@4><😀@6><c@8>\"]");
check("replace function named groups", both('(?<v>[éö])', 'g', r => BMP.replace(r, positions)), "[\"h<é@1>llo w<ö@7>rld <é@12>\",\"h<é@1>llo w<ö@7>rld <é@12>\"]");
check("replace sticky", both('l', 'y', r => { r.lastIndex = 2; return [BMP.replace(r, '#'), r.lastIndex]; }), "[[\"hé#lo wörld é\",3],[\"hé#lo wörld é\",3]]");
check("replace sticky miss", both('l', 'y', r => { r.lastIndex = 1; return [BMP.replace(r, '#'), r.lastIndex]; }), "[[\"héllo wörld é\",0],[\"héllo wörld é\",0]]");
check("replace empty u", both('(?:)', 'gu', r => 'a😀é'.replace(r, '-')), "[\"-a-😀-é-\",\"-a-😀-é-\"]");
check("replace empty v", both('(?:)', 'gv', r => 'a😀é'.replace(r, '-')), "[\"-a-😀-é-\",\"-a-😀-é-\"]");
check("replace empty BMP", both('(?:)', 'g', r => 'aéb'.replace(r, '-')), "[\"-a-é-b-\",\"-a-é-b-\"]");
check("replace empty lone", both('(?:)', 'gu', r => LONE.replace(r, '-')), "[\"-a-\\ud800-b-\\udc00-c-\",\"-a-\\ud800-b-\\udc00-c-\"]");
check("replaceAll function", both('ö|é', 'g', r => BMP.replaceAll(r, positions)), "[\"h<é@1>llo w<ö@7>rld <é@12>\",\"h<é@1>llo w<ö@7>rld <é@12>\"]");
check("split", both('[éö ]', '', r => BMP.split(r)), "[[\"h\",\"llo\",\"w\",\"rld\",\"\",\"\"],[\"h\",\"llo\",\"w\",\"rld\",\"\",\"\"]]");
check("split limit", both('l', '', r => BMP.split(r, 2)), "[[\"hé\",\"\"],[\"hé\",\"\"]]");
check("split empty u", both('(?:)', 'u', r => 'é😀x'.split(r)), "[[\"é\",\"😀\",\"x\"],[\"é\",\"😀\",\"x\"]]");
check("split empty BMP", both('', '', r => 'aéb'.split(r)), "[[\"a\",\"é\",\"b\"],[\"a\",\"é\",\"b\"]]");
check("split captures", both('(é|ö)', '', r => BMP.split(r)), "[[\"h\",\"é\",\"llo w\",\"ö\",\"rld \",\"é\",\"\"],[\"h\",\"é\",\"llo w\",\"ö\",\"rld \",\"é\",\"\"]]");
check("split lone", both('b', '', r => LONE.split(r)), "[[\"a\\ud800\",\"\\udc00c\"],[\"a\\ud800\",\"\\udc00c\"]]");
check("search", both('ö', 'g', r => { r.lastIndex = 9; return [BMP.search(r), r.lastIndex]; }), "[[7,9],[7,9]]");
check("search astral", both('c', 'u', r => ASTRAL.search(r)), "[8,8]");
check("symbol replace", both('l', 'g', r => RegExp.prototype[Symbol.replace].call(r, BMP, positions)), "[\"hé<l@2><l@3>o wör<l@9>d é\",\"hé<l@2><l@3>o wör<l@9>d é\"]");
check("symbol match", both('ö|é', 'g', r => RegExp.prototype[Symbol.match].call(r, BMP)), "[[\"é\",\"ö\",\"é\"],[\"é\",\"ö\",\"é\"]]");
check("symbol matchAll", both('l', 'g', r => [...RegExp.prototype[Symbol.matchAll].call(r, BMP)].map(m => m.index)), "[[2,3,9],[2,3,9]]");
check("symbol search", both('d', '', r => RegExp.prototype[Symbol.search].call(r, BMP)), "[10,10]");
check("symbol split", both('ö', '', r => RegExp.prototype[Symbol.split].call(r, BMP)), "[[\"héllo w\",\"rld é\"],[\"héllo w\",\"rld é\"]]");
check("literal exec index", (() => { const r = /ö/g; r.exec(BMP); return [r.lastIndex, /ö/.exec(BMP).index]; })(), "[8,7]");
check("literal replace", [BMP.replace(/l/g, positions), BMP.replace(/é/, '#')], "[\"hé<l@2><l@3>o wör<l@9>d é\",\"h#llo wörld é\"]");
check("string search", [BMP.search('l'), ASTRAL.search('c'), LONE.search('c')], "[2,8,4]");
check("string match", (() => { const m = BMP.match('l+'); return [m.index, m[0]]; })(), "[2,\"ll\"]");
check("string replace positions", [BMP.replace('l', positions), BMP.replaceAll('l', positions), ASTRAL.replaceAll('😀', positions)], "[\"hé<l@2>lo wörld é\",\"hé<l@2><l@3>o wör<l@9>d é\",\"a<😀@1>b<😀@4><😀@6>c\"]");
check("string replaceAll empty", ['ab'.replaceAll('', '-'), 'é'.replaceAll('', '-'), 'a😀'.replaceAll('', '-'), 'é😀'.replaceAll('', positions)], "[\"-a-b-\",\"-é-\",\"-a-\\ud83d-\\ude00-\",\"<@0>é<@1>\\ud83d<@2>\\ude00<@3>\"]");
check("long non-ASCII exec loop", (() => {
    const s = 'é'.repeat(5000) + 'x' + '😀'.repeat(3000) + 'x' + 'ü'.repeat(4000) + 'x';
    const r = /x/g; const out = [];
    let m; while ((m = r.exec(s))) out.push(m.index, r.lastIndex);
    return [out, s.replace(/x/g, (m, i) => i).length, [...s.matchAll(/x/gu)].map(m => m.index)];
  })(), "[[5000,5001,11001,11002,15002,15003],15014,[5000,11001,15002]]");
check("word classes no flags", ['\\w', '\\W', '\\d', '\\D', '\\s', '\\S', '[\\w]', '[^\\W]', '[\\W]', '[\\s]', '[\\S]', '[^\\S]'].map(p => testChars('^' + p + '$', '', CHARS)), "[\"00000000000011111110\",\"11111111111100000001\",\"00000000000000000100\",\"11111111111111111011\",\"00001111001000000001\",\"11110000110111111110\",\"00000000000011111110\",\"00000000000011111110\",\"11111111111100000001\",\"00001111001000000001\",\"11110000110111111110\",\"00001111001000000001\"]");
check("word classes i", ['\\w', '\\W', '[\\w]', '[^\\W]', '[\\W]', '[^\\w]'].map(p => testChars('^' + p + '$', 'i', CHARS)), "[\"00000000000011111110\",\"11111111111100000001\",\"00000000000011111110\",\"00000000000011111110\",\"11111111111100000001\",\"11111111111100000001\"]");
check("word classes u", ['\\w', '\\W', '\\d', '\\s', '\\S', '[\\w]', '[^\\W]', '[\\W]', '[^\\S]'].map(p => testChars('^' + p + '$', 'u', CHARS)), "[\"00000000000011111110\",\"11111111111100000001\",\"00000000000000000100\",\"00001111001000000001\",\"11110000110111111110\",\"00000000000011111110\",\"00000000000011111110\",\"11111111111100000001\",\"00001111001000000001\"]");
check("word classes iu", ['\\w', '\\W', '[\\w]', '[^\\W]', '[\\W]', '[^\\w]', '[\\w\\d]', '[\\W\\d]'].map(p => testChars('^' + p + '$', 'iu', CHARS)), "[\"00000000000111111110\",\"11111111111000000001\",\"00000000000111111110\",\"00000000000111111110\",\"11111111111000000001\",\"11111111111000000001\",\"00000000000111111110\",\"11111111111000000101\"]");
check("word classes v", ['\\w', '\\W', '[\\w]', '[\\W]', '[\\s--\\u00a0]', '[\\w--\\d]', '[\\W--\\s]', '[\\s&&\\p{Zs}]'].map(p => testChars('^' + p + '$', 'v', CHARS)), "[\"00000000000011111110\",\"11111111111100000001\",\"00000000000011111110\",\"11111111111100000001\",\"00000111001000000001\",\"00000000000011111010\",\"11110000110100000000\",\"00001000001000000001\"]");
check("word classes iv", ['\\w', '\\W', '[\\w]', '[\\W]', '[\\w--\\d]', '[^\\W]'].map(p => testChars('^' + p + '$', 'iv', CHARS)), "[\"00000000000111111110\",\"11111111111000000001\",\"00000000000111111110\",\"11111111111000000001\",\"00000000000111111010\",\"00000000000111111110\"]");
check("boundaries", ['', 'i', 'u', 'iu', 'v', 'iv'].map(f => ['a\\b', 'a\\B', '\\b', '\\B'].map(p => testChars(p, f, CHARS.map(c => 'a' + c))).join(' ')), "[\"11111111111100000011 00000000000011111110 11111111111111111111 11111111111111111111\",\"11111111111100000011 00000000000011111110 11111111111111111111 11111111111111111111\",\"11111111111100000011 00000000000011111110 11111111111111111111 11111111111111111111\",\"11111111111000000011 00000000000111111110 11111111111111111111 11111111111111111111\",\"11111111111100000011 00000000000011111110 11111111111111111111 11111111111111111111\",\"11111111111000000011 00000000000111111110 11111111111111111111 11111111111111111111\"]");
check("boundary search", [['\\b', 'iu'], ['\\b', 'i'], ['a\\B', 'iu'], ['a\\B', 'i'], ['\\bK', 'iu'], ['\\bé', 'u']].map(([p, f]) => ['ſ', 'aſ', 'K', 'aK', 'é', 'aé'].map(s => s.search(new RegExp(p, f)))), "[[0,0,0,0,-1,0],[-1,0,0,0,-1,0],[-1,0,-1,0,-1,-1],[-1,-1,-1,0,-1,-1],[-1,-1,0,-1,-1,-1],[-1,-1,-1,-1,-1,1]]");
check("boundary replace", ['', 'u', 'iu'].map(f => 'naïve café ſK'.replace(new RegExp('\\b', 'g' + f), '|')), "[\"|na|ï|ve| |caf|é ſ|K|\",\"|na|ï|ve| |caf|é ſ|K|\",\"|na|ï|ve| |caf|é |ſK|\"]");
check("word match", ['', 'u', 'iu'].map(f => 'naïve café_crème ſtraße Kelvin ٣٤'.match(new RegExp('\\w+', 'g' + f))), "[[\"na\",\"ve\",\"caf\",\"_cr\",\"me\",\"tra\",\"e\",\"Kelvin\"],[\"na\",\"ve\",\"caf\",\"_cr\",\"me\",\"tra\",\"e\",\"Kelvin\"],[\"na\",\"ve\",\"caf\",\"_cr\",\"me\",\"ſtra\",\"e\",\"Kelvin\"]]");
check("space split", 'a\u00a0b\ufeffc\u2028d\u3000e\u180ef\u0085g\u200bh'.split(/\s/), "[\"a\",\"b\",\"c\",\"d\",\"e᠎fg​h\"]");
check("space classes", ['\\s+', '\\S+', '[\\s\\d]+', '[^\\s]+'].map(p => 'x\u00a0\ufeff y\u2029z\u180e'.match(new RegExp(p, 'g'))), "[[\" ﻿ \",\" \"],[\"x\",\"y\",\"z᠎\"],[\" ﻿ \",\" \"],[\"x\",\"y\",\"z᠎\"]]");
check("digits", ['\\d+', '\\D+', '[\\d]+', '[^\\D]+'].map(p => '٣٤ 12 ３x9'.match(new RegExp(p, 'gu'))), "[[\"12\",\"9\"],[\"٣٤ \",\" ３x\"],[\"12\",\"9\"],[\"12\",\"9\"]]");
check("case folding", [/é/i.test('É'), /É/i.test('é'), /σ/i.test('Σ'), /ς/iu.test('σ'), /s/i.test('ſ'), /s/iu.test('ſ'), /k/i.test('\u212a'), /k/iu.test('\u212a'), /[a-z]/i.test('ſ'), /[a-z]/iu.test('ſ'), /\p{Lu}/u.test('É'), /\p{L}+/u.exec('été!')[0]], "[true,true,true,true,false,true,false,true,false,true,true,\"été\"]");

if (failures) throw new Error(failures + ' regexp unicode index checks failed');
console.log('regexp unicode indices ok');
