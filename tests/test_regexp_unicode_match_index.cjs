const assert = require('node:assert');

for (let iteration = 0; iteration < 100; iteration++) {
  for (const prefix of ['', 'é', '😀é']) {
    const subject = prefix + 'alpha-beta😀';
    const position = prefix.length;
    assert.strictEqual(/alpha/.exec(subject).index, position);
    assert.strictEqual(/(alpha)/.exec(subject).index, position);
    assert.strictEqual(new RegExp('(alpha)').exec(subject).index, position);
    assert.strictEqual(subject.match('(alpha)').index, position);
    assert.strictEqual(subject.search(/(alpha)/), position);
    assert.strictEqual(/(?<part>alpha)/.exec(subject).index, position);

    let offset;
    assert.strictEqual(subject.replace(/(alpha)/, (match, capture, at, input) => {
      assert.strictEqual(match, 'alpha');
      assert.strictEqual(capture, 'alpha');
      assert.strictEqual(input, subject);
      offset = at;
      return 'A';
    }), prefix + 'A-beta😀');
    assert.strictEqual(offset, position);
    assert.strictEqual(subject.replace(/(alpha)/, '<$1>'), prefix + '<alpha>-beta😀');
    assert.strictEqual(subject.replace(/(alpha)/, "$`"), prefix + prefix + '-beta😀');
    assert.strictEqual(subject.replace(/(alpha)/, "$'"), prefix + '-beta😀-beta😀');
  }
}

// An overridden exec already supplies UTF-16 indices; do not reinterpret them.
const custom = /alpha/;
custom.exec = () => Object.assign(['alpha'], { index: 3, input: '😀éalphaz' });
let customOffset;
assert.strictEqual('😀éalphaz'.replace(custom, (match, at) => {
  customOffset = at;
  return 'X';
}), '😀éXz');
assert.strictEqual(customOffset, 3);
console.log('PASS UTF-16 RegExp match indices and replacement offsets');
