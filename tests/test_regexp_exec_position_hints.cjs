function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

function naiveIndices(s, re) {
  const out = [];
  for (let i = 0; i < s.length; i++) {
    const sticky = new RegExp(re.source, 'y' + re.flags.replace(/[gy]/g, ''));
    sticky.lastIndex = i;
    const m = sticky.exec(s);
    if (m) { out.push(i + ':' + m[0]); i += Math.max(m[0].length, 1) - 1; }
  }
  return out;
}

const line = 'línea número 42 con acentos é 😀 ñ\n';
const text = line.repeat(300);

{
  const lineRe = /^.*$/gm;
  const got = [];
  let m;
  while ((m = lineRe.exec(text))) {
    const w = m[0].match(/\d+/);
    const e = /é/.exec(m[0]);
    got.push(m.index + ':' + (w ? w.index : -1) + ':' + (e ? e.index : -1));
    if (m[0].length === 0) lineRe.lastIndex++;
  }
  const expected = [];
  let at = 0;
  for (let i = 0; i < 300; i++) { expected.push(at + ':13:28'); at += line.length; }
  expected.push(at + ':-1:-1');
  check('nested exec loop', got, expected);
}

{
  const s1 = 'héllo wörld 123 😀 '.repeat(200), s2 = 'naïve café 456 '.repeat(200);
  const a = /\d+/g, b = /\d+/g;
  const ga = [], gb = [];
  let m, n;
  while ((m = a.exec(s1)) && (n = b.exec(s2))) { ga.push(m.index + ':' + a.lastIndex); gb.push(n.index + ':' + b.lastIndex); }
  const ea = [], eb = [];
  for (let i = 0; i < 200; i++) {
    ea.push((i * 19 + 12) + ':' + (i * 19 + 15));
    eb.push((i * 15 + 11) + ':' + (i * 15 + 14));
  }
  check('alternating subjects a', ga, ea);
  check('alternating subjects b', gb, eb);
}

{
  const s = 'ä😀b😀c'.repeat(50);
  const re = /😀./gu;
  const got = [];
  let m;
  while ((m = re.exec(s))) { got.push(m.index + ':' + m[0]); 'x😀'.replace(/😀/u, 'y'); }
  check('surrogate exec loop', got, naiveIndices(s, /😀./u));
}

{
  const s = 'éa'.repeat(10);
  const re = /a/g;
  re.exec(s);
  re.lastIndex = 1;
  check('rewind lastIndex', re.exec(s).index, 1);
  re.lastIndex = 15;
  check('jump lastIndex', re.exec(s).index, 15);
  re.lastIndex = 3;
  check('back lastIndex', re.exec(s).index, 3);
  const sticky = /a/y;
  sticky.lastIndex = 5;
  check('sticky lastIndex', [sticky.test(s), sticky.lastIndex], [true, 6]);
}

console.log('ok');
