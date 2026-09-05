const assert = {
  equal(actual, expected) {
    if (actual !== expected) {
      throw new Error(`expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
    }
  },
};

for (const prefix of ['', 'abc', 'é', '€', '😀', 'é😀€']) {
  const input = prefix + 'xy!';
  for (const flags of ['', 'u', 'g', 'gu']) {
    const regexp = new RegExp('(?<value>xy)', flags);
    const result = regexp.exec(input);
    assert.equal(result.index, prefix.length);
    assert.equal(result[0], 'xy');
    assert.equal(result.groups.value, 'xy');
    assert.equal(result.input, input);

    assert.equal(input.replace(regexp, 'Z'), prefix + 'Z!');
    assert.equal(input.replace(regexp, (match, capture, offset) => {
      assert.equal(match, 'xy');
      assert.equal(capture, 'xy');
      assert.equal(offset, prefix.length);
      return 'Z';
    }), prefix + 'Z!');
  }
}

console.log('regexp exec UTF-16 index ok');
