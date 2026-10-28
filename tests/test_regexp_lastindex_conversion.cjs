function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
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

{
  const re = new RegExp('q' + 'x(y)z'.repeat(3) + Math.random().toString(36).slice(2), 'g');
  re.lastIndex = { valueOf() { re.compile('b', 'g'); churn(); return 0; } };
  const m = re.exec('ab');
  check('compile in lastIndex.valueOf', [m && m[0], m && m.index, re.lastIndex, re.source], ['b', 1, 2, 'b']);
}

{
  const re = /a/y;
  re.lastIndex = { valueOf() { re.compile('b', ''); return 1; } };
  check('compile drops sticky', re.exec('ab')[0], 'b');
}

function throwsType(fn) {
  try { fn(); } catch (e) { return e instanceof TypeError; }
  return false;
}

check('bigint lastIndex global', throwsType(() => { const re = /a/g; re.lastIndex = 1n; re.exec('aa'); }), true);
check('bigint lastIndex plain', throwsType(() => { const re = /a/; re.lastIndex = 1n; re.exec('aa'); }), true);
check('bigint lastIndex test', throwsType(() => { const re = /a/; re.lastIndex = 1n; re.test('aa'); }), true);
check('bigint lastIndex replace', throwsType(() => { const re = /a/; re.lastIndex = 1n; 'aa'.replace(re, 'b'); }), true);
check('symbol lastIndex', throwsType(() => { const re = /a/; re.lastIndex = Symbol(); re.exec('a'); }), true);

{
  const counts = [];
  for (const kind of ['exec', 'test', 'match', 'replace', 'replace-fn', 'search', 'split']) {
    let calls = 0;
    const re = /a/;
    re.lastIndex = { valueOf() { calls++; return 0; } };
    if (kind === 'exec') re.exec('a');
    else if (kind === 'test') re.test('a');
    else if (kind === 'match') 'a'.match(re);
    else if (kind === 'replace') 'a'.replace(re, 'b');
    else if (kind === 'replace-fn') 'a'.replace(re, () => 'b');
    else if (kind === 'search') 'a'.search(re);
    else 'a'.split(re);
    counts.push(kind + '=' + calls);
  }
  check('valueOf calls', counts, ['exec=1', 'test=1', 'match=1', 'replace=1', 'replace-fn=1', 'search=0', 'split=0']);
}

{
  const re = /a/;
  re.lastIndex = { valueOf() { throw new RangeError('boom'); } };
  let name = 'none';
  try { re.test('a'); } catch (e) { name = e.name; }
  check('valueOf throws', name, 'RangeError');
}

{
  const re = /a/g;
  re.lastIndex = '1';
  const m = re.exec('aaa');
  check('string lastIndex', [m.index, re.lastIndex], [1, 2]);
  re.lastIndex = 2 ** 53 + 10;
  check('huge lastIndex', [re.exec('aaa'), re.lastIndex], [null, 0]);
  re.lastIndex = -5;
  check('negative lastIndex', re.exec('aaa').index, 0);
}

{
  const re = /b/;
  Object.defineProperty(re, 'lastIndex', { writable: false });
  check('search keeps non-writable lastIndex', 'ab'.search(re), 1);
}

console.log('ok');
