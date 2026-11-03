const assert = require('node:assert');

const sources = [
  "'", 'ba', '^ba', '^a\\+', '\\.', '\\\\', '\\t', '\\/', '\\-', '\\#', '\\[\\]', '\\?x', '\\n', 'a\\.b', '\\$',
  '^\\.', 'é', '^é', 'テ', '\\(', 'x\\v', 'd1', 'NQ_VQ', '%2S',
];
const flagSets = ['', 'g', 'y', 'm', 'i', 'gm', 'u', 'gu', 's', 'd'];
const strings = [
  '', 'ba', 'bar', 'abar', 'b', 'a.b', 'a\\b', 'x\ty', '/a/b/', 'a-b', '#x#', '[]x', '?x?x', 'l1\nl2', 'a+b', '$5',
  '.x', 'été', 'テキスト', '(a)', 'x\vy', "it's", 'd1d2d1', 'NQ_VQ NQ_VQ', '%2s%2S', 'x\nba', 'BA',
  'a,'.repeat(40) + 'ba',
];

function observe(re, s) {
  re.lastIndex = 0;
  const exec = re.exec(s);
  const afterExec = re.lastIndex;
  re.lastIndex = 0;
  const test = re.test(s);
  const afterTest = re.lastIndex;
  re.lastIndex = 0;
  return JSON.stringify({
    exec: exec && [...exec],
    index: exec && exec.index,
    indices: exec && exec.indices && [...exec.indices],
    afterExec,
    test,
    afterTest,
    replace: s.replace(re, '<$&>'),
    replaceFn: s.replace(re, (m) => `[${m}]`),
    split: s.split(re),
    match: s.match(re),
    search: s.search(re),
  });
}

let compared = 0;
for (const source of sources) {
  for (const flags of flagSets) {
    let fast;
    try { fast = new RegExp(source, flags); } catch { continue; }
    const reference = new RegExp(`(?:${source})`, flags);
    for (const s of strings) {
      assert.strictEqual(observe(fast, s), observe(reference, s), `/${source}/${flags} on ${JSON.stringify(s)}`);
      compared++;
    }
  }
}

assert.ok(compared > 4000);
console.log('ok');
