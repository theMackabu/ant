const assert = require("node:assert");

// Without the u or v flag a RegExp matches UTF-16 code units, so a surrogate
// pair is two characters. Ant's matcher works on code points; these cases
// cover the code unit view non-unicode RegExps now match against.

const E = "\u{1F4C1}";
const G = "\u{1F600}";
const s = "a" + E + "b";
const H = "\ud83d", L = "\udcc1";

// Svelte blanks out template text with replace(/\S/g, " ") and then indexes the
// original source with the result; dropping a code unit per pair shifted every
// later position.
assert.strictEqual(s.replace(/\S/g, " ").length, s.length);
assert.strictEqual((E + " " + G).replace(/\S/g, " "), "     ");
assert.strictEqual(s.replace(/./g, "-"), "----");
assert.strictEqual(s.replace(/[^]/g, "-"), "----");
assert.strictEqual(s.replace(/[^a]/g, "-"), "a---");
assert.strictEqual(s.replace(/[^A]/gi, "-"), "a---");
assert.strictEqual(s.replaceAll(/\S/g, "-"), "----");
assert.strictEqual(s.replace(/./gu, "-"), "---");

assert.deepStrictEqual(s.match(/\S/g), ["a", H, L, "b"]);
assert.deepStrictEqual([...s.matchAll(/./g)].map(m => [m[0], m.index]), [["a", 0], [H, 1], [L, 2], ["b", 3]]);
assert.deepStrictEqual(s.match(/./gu), ["a", E, "b"]);

{
  const m = s.match(/a(.)/);
  assert.deepStrictEqual([m[0], m[1], m.index], ["a" + H, H, 0]);
  assert.strictEqual(m.input, s);
}

{
  const re = /\S/g;
  const seen = [];
  let m;
  while ((m = re.exec(s))) seen.push([m[0].length, m.index, re.lastIndex]);
  assert.deepStrictEqual(seen, [[1, 0, 1], [1, 1, 2], [1, 2, 3], [1, 3, 4]]);
}

{
  const re = /\S/y;
  re.lastIndex = 2;
  const m = re.exec(s);
  assert.deepStrictEqual([m[0], m.index, re.lastIndex], [L, 2, 3]);
  const g = /./g;
  g.lastIndex = 2;
  assert.deepStrictEqual([g.exec(s)[0], g.lastIndex], [L, 3]);
}

assert.deepStrictEqual([/^.$/.test(E), /^..$/.test(E), /^.$/u.test(E)], [false, true, true]);
assert.strictEqual(s.replace(/\S/g, (m, i, str) => "<" + m.length + ":" + i + ":" + (str === s) + ">"),
  "<1:0:true><1:1:true><1:2:true><1:3:true>");
assert.strictEqual(s.replace(/(.)(.)/g, "[$2$1]"), "[" + H + "a][b" + L + "]");
assert.strictEqual(s.replace(/a(.)/g, "$'|$`|$&"), L + "b||a" + E + "b");
assert.deepStrictEqual(s.split(/(?:)/), ["a", H, L, "b"]);
assert.deepStrictEqual(s.split(new RegExp("(" + L + ")")), ["a" + H, L, "b"]);
assert.deepStrictEqual(s.split(/(?:)/, 2), ["a", H]);

// Surrogate escapes and literals in the pattern.
assert.strictEqual(s.search(/\udcc1/), 2);
assert.strictEqual(/\ud83d/.test(s), true);
assert.strictEqual(/\ud83d\udcc1/.test(s), true);
assert.strictEqual(/\ud83d\udcc1/u.test(s), true);
assert.deepStrictEqual([/\ud83d/u.test(E), /\ud83d/u.test(H), /^[\ud83d\udcc1]$/u.test(E), /^[\ud83d\udcc1]$/.test(E)],
  [false, true, true, false]);
assert.strictEqual((E + E).replace(new RegExp(E + "+", "g"), "X"), "XX");
assert.strictEqual(s.replace(new RegExp("[" + E + "]", "g"), "X"), "aXXb");
assert.strictEqual(s.replace(new RegExp("[" + E + "]", "gu"), "X"), "aXb");
assert.strictEqual(s.replace(new RegExp(E, "g"), "X"), "aXb");
assert.deepStrictEqual([new RegExp(H).test(E), new RegExp(H, "u").test(E), new RegExp(H, "y").test(E)], [true, false, true]);
assert.strictEqual(s.replace(/(?<=\ud83d)./g, "L"), "a" + H + "Lb");
assert.strictEqual((E + E).replace(/(.)\udcc1\1/g, "Z"), "Z" + L);
assert.deepStrictEqual([.../b(.)/d.exec("a" + E + "b" + G).indices], [[3, 5], [4, 5]]);
assert.throws(() => new RegExp("[" + G + "-" + E + "]"), SyntaxError);
// Without u, \u{...} is an identity escape and a quantifier (Annex B).
assert.deepStrictEqual([/\u{1F4C1}/.test(E), /\u{1F4C1}/.test("u{1F4C1}"), /^\u{3}$/.test("uuu"), /\u{1F4C1}/u.test(E)],
  [false, true, true, true]);

// RegExp.input is the subject itself, never the split form.
/\udcc1(.)/.test(s);
assert.strictEqual(RegExp.input, s);
assert.deepStrictEqual([RegExp.leftContext, RegExp.$1, RegExp.rightContext], ["a" + H, "b", ""]);

// The generic \\replace / \\split paths slice the subject by UTF-16 offsets.
class Sub extends RegExp {
  exec(str) { return super.exec(str); }
}
const t = "a" + E + "b" + G + "c";
assert.strictEqual(t.replace(new Sub("\\S", "g"), "-"), "-------");
assert.strictEqual(t.replace(new Sub("(.)(.)", "g"), "[$2$1]"), "[" + H + "a][b" + L + "][\ude00\ud83d]c");
assert.deepStrictEqual(t.split(new Sub("")).map(x => x.length), [1, 1, 1, 1, 1, 1, 1]);
assert.deepStrictEqual(t.split(new Sub(L)), ["a" + H, "b" + G + "c"]);
// $' starts where the match ends in code units, also with an overridden exec.
const ownExec = (src, flags) => {
  const re = new RegExp(src, flags);
  re.exec = function (str) { return RegExp.prototype.exec.call(this, str); };
  return re;
};
assert.strictEqual(t.replace(new Sub(E), "[$`|$&|$']"), "a[a|" + E + "|b" + G + "c]b" + G + "c");
assert.strictEqual(t.replace(new Sub(".b", "g"), "($')"), "a" + H + "(" + G + "c)" + G + "c");
assert.strictEqual(t.replace(new Sub(E + "b" + G, "u"), "<$'>"), "a<c>c");
assert.strictEqual(t.replace(ownExec("b.", "g"), "[$'|$`]"), "a" + E + "[\ude00c|a" + E + "]\ude00c");
assert.strictEqual(t.replace(ownExec(G, "gu"), "{$&$'}"), "a" + E + "b{" + G + "c}c");

// Long subjects take the cached view; interleave two of them.
const long = ("x".repeat(300) + E + "y—".repeat(100) + G).repeat(20);
const other = "z—".repeat(200) + G;
assert.strictEqual(long.replace(/./g, "-").length, long.length);
assert.strictEqual(long.match(/[\ud800-\udbff]/g).length, 40);
{
  const re = /[^]/y;
  let n = 0;
  while (re.exec(long)) n++;
  assert.strictEqual(n, long.length);
}
{
  const re = /./g;
  for (let i = 0; i < 50; i++) {
    re.lastIndex = 301;
    assert.strictEqual(re.exec(long)[0], L);
    re.lastIndex = other.length - 1;
    assert.strictEqual(re.exec(other)[0], "\ude00");
  }
}

// A replace callback that allocates heavily must not lose the subject.
{
  const junk = [];
  const big = ("q" + E + "é").repeat(200);
  const out = big.replace(/./g, (m) => {
    for (let i = 0; i < 200; i++) junk.push("s" + i + "—".repeat(i % 20));
    if (junk.length > 20000) junk.length = 0;
    return m.charCodeAt(0) >= 0xd800 && m.charCodeAt(0) <= 0xdfff ? "S" : "c";
  });
  assert.strictEqual(out, "cSSc".repeat(200));
}

console.log("ok");
