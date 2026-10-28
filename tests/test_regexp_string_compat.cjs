const cases = {
  replaceDollar: () => ['abc'.replace('b', '$&$&'), 'abcb'.replaceAll('b', '[$`|$&|$\'|$1|$$]'), 'aéb'.replace('é', '<$`>'), 'a1c'.replace(1, 'x'), 'abc'.replace('b', 5), 'abc'.replace('b'), 'aXbX'.replaceAll('X', '$<n>')],
  flags: () => ['gg', 'q', 'uv', 'gimsyd', 'G', 'dgimsuy'].map(f => { try { return String(new RegExp('x', f)); } catch (e) { return e.name; } }),
  literalFlags: () => ['/x/gg', '/x/uv', '/x/gimsyd', '/x'].map(src => { try { return String(eval(src)); } catch (e) { return e.name; } }),
  compileFlags: () => { try { /x/.compile('y', 'gg'); return 'ok'; } catch (e) { return e.name; } },
  lineTerminators: () => {
    const subj = ['a\nb', 'a\rb', 'a\r\nb', 'a b', 'a b', 'a\u0085b', 'a\vb', 'ab\n', 'ab\r\n'];
    const pats = [['.', ''], ['.', 's'], ['^b', 'm'], ['a$', ''], ['a$', 'm'], ['b$', ''], ['^', 'gm'], ['$', 'gm'], ['^$', 'gm'], ['^.+$', 'gm']];
    const out = [];
    for (const [p, f] of pats) for (const s of subj) {
      const r = new RegExp(p, f);
      out.push(f.includes('g') ? [...s.matchAll(r)].map(m => m.index + ':' + m[0]).join(',') : String(r.exec(s) && r.exec(s).index));
    }
    return out;
  },
  legacyCaseless: () => {
    const chars = ['ß', 'ẞ', 'Ω', 'ω', 'Ω', 'Å', 'å', 'Å', 'a'];
    const pats = ['ß', 'ẞ', 'Ω', 'ω', 'Ω', 'Å', 'å', 'Å', '[ß]', '[^ß]', '[ω]', '[^ω]', '[a-zå]', 'xß+'];
    const out = [];
    for (const f of ['i', 'iu']) for (const p of pats) {
      const r = new RegExp('^' + p + '$', f);
      out.push(p + '/' + f + ':' + chars.filter(c => r.test(p.startsWith('x') ? 'x' + c + c : c)).join(''));
    }
    return out;
  },
  wellFormed: () => ['', 'abc', 'é😀', 'a\ud83db', '\udc00', '\ud83d' + '\ude00', 'x\ude00\ud83dy'].map(s => [s.isWellFormed(), s.toWellFormed().split('').map(c => c.charCodeAt(0).toString(16)).join(' ')]),
  output: () => {
    const { spawnSync } = require('child_process');
    const script = 'console.log("a\\ud83db\\udc00c"); process.stdout.write("x\\udc00y\\n"); console.error("e\\ud800");';
    const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => !k.startsWith('DYLD_')));
    const r = spawnSync(process.execPath, ['-e', script], { env });
    return [[...r.stdout].join(','), [...r.stderr].join(',')];
  },
};

const expected = {
  replaceDollar: ["abbc", "a[a|b|cb|$1|$]c[abc|b||$1|$]", "a<a>b", "axc", "a5c", "aundefinedc", "a$<n>b$<n>"],
  flags: ["SyntaxError", "SyntaxError", "SyntaxError", "/x/dgimsy", "SyntaxError", "/x/dgimsuy"],
  literalFlags: ["SyntaxError", "SyntaxError", "/x/dgimsy", "SyntaxError"],
  compileFlags: "SyntaxError",
  lineTerminators: ["0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "2", "2", "3", "2", "2", "null", "null", "null", "null", "null", "null", "null", "null", "null", "null", "null", "null", "null", "0", "0", "0", "0", "0", "null", "null", "null", "null", "2", "2", "3", "2", "2", "2", "2", "null", "null", "0:,2:", "0:,2:", "0:,2:,3:", "0:,2:", "0:,2:", "0:", "0:", "0:,3:", "0:,3:,4:", "1:,3:", "1:,3:", "1:,2:,4:", "1:,3:", "1:,3:", "3:", "3:", "2:,3:", "2:,3:,4:", "", "", "2:", "", "", "", "", "3:", "3:,4:", "0:a,2:b", "0:a,2:b", "0:a,3:b", "0:a,2:b", "0:a,2:b", "0:a\u0085b", "0:a\u000bb", "0:ab", "0:ab"],
  legacyCaseless: ["\u00df/i:\u00df", "\u1e9e/i:\u1e9e", "\u2126/i:\u2126", "\u03c9/i:\u03c9\u03a9", "\u03a9/i:\u03c9\u03a9", "\u212b/i:\u212b", "\u00e5/i:\u00e5\u00c5", "\u00c5/i:\u00e5\u00c5", "[\u00df]/i:\u00df", "[^\u00df]/i:\u1e9e\u2126\u03c9\u03a9\u212b\u00e5\u00c5a", "[\u03c9]/i:\u03c9\u03a9", "[^\u03c9]/i:\u00df\u1e9e\u2126\u212b\u00e5\u00c5a", "[a-z\u00e5]/i:\u00e5\u00c5a", "x\u00df+/i:\u00df", "\u00df/iu:\u00df\u1e9e", "\u1e9e/iu:\u00df\u1e9e", "\u2126/iu:\u2126\u03c9\u03a9", "\u03c9/iu:\u2126\u03c9\u03a9", "\u03a9/iu:\u2126\u03c9\u03a9", "\u212b/iu:\u212b\u00e5\u00c5", "\u00e5/iu:\u212b\u00e5\u00c5", "\u00c5/iu:\u212b\u00e5\u00c5", "[\u00df]/iu:\u00df\u1e9e", "[^\u00df]/iu:\u2126\u03c9\u03a9\u212b\u00e5\u00c5a", "[\u03c9]/iu:\u2126\u03c9\u03a9", "[^\u03c9]/iu:\u00df\u1e9e\u212b\u00e5\u00c5a", "[a-z\u00e5]/iu:\u212b\u00e5\u00c5a", "x\u00df+/iu:\u00df\u1e9e"],
  wellFormed: [[true, ""], [true, "61 62 63"], [true, "e9 d83d de00"], [false, "61 fffd 62"], [false, "fffd"], [true, "d83d de00"], [false, "78 fffd fffd 79"]],
  output: ["97,239,191,189,98,239,191,189,99,10,120,239,191,189,121,10", "101,239,191,189,10"],
};

for (const name in cases) {
  const actual = JSON.stringify(cases[name]());
  const want = JSON.stringify(expected[name]);
  if (actual !== want) throw new Error(name + ': ' + actual + ' !== ' + want);
}
console.log('ok');
