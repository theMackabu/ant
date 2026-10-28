function check(name, actual, expected) {
  if (actual !== expected) throw new Error(`${name}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

function churn() {
  const keep = [];
  for (let r = 0; r < 20; r++) {
    const a = [];
    for (let i = 0; i < 50000; i++) a.push({ i, s: 's' + i });
    keep.push(a.length);
  }
  return keep.length;
}

for (let round = 0; round < 3; round++) {
  const name = 'k' + Math.random().toString(36).slice(2);
  const re = new RegExp('(?<' + name + '>a)(b)?', 'g');
  let first = true;
  const out = 'aaab'.replace(re, function () {
    if (first) {
      first = false;
      re.compile('zz', 'g');
      churn();
    }
    const groups = arguments[arguments.length - 1];
    return '[' + Object.keys(groups).join() + '=' + groups[name] + ':' + arguments[2] + ']';
  });
  const expected = `[${name}=a:undefined]`.repeat(2) + `[${name}=a:b]`;
  check('replace callback recompile', out, expected);
  check('recompiled source', re.source, 'zz');
}

{
  const re = /(\d)/g;
  const out = '1 2 3'.replace(re, (m, d) => {
    re.compile('x', 'g');
    churn();
    return '<' + d + '>';
  });
  check('unnamed replace callback recompile', out, '<1> <2> <3>');
}

{
  const name = 'n' + Math.random().toString(36).slice(2);
  const re = new RegExp('(?<' + name + '>.)', 'g');
  const seen = [];
  for (const m of 'xyz'.matchAll(re)) {
    re.compile('q', 'g');
    churn();
    seen.push(m.groups[name]);
  }
  check('matchAll recompile', seen.join(), 'x,y,z');
}

{
  const re = /,/;
  const parts = 'a,b,c'.split({
    [Symbol.split](s, lim) { re.compile(';'); churn(); return RegExp.prototype[Symbol.split].call(/,/, s, lim); },
  });
  check('split recompile', parts.join('|'), 'a|b|c');
}

console.log('ok');
