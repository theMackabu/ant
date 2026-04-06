function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function gcPressure() {
  const junk = [];
  for (let i = 0; i < 256; i++) {
    junk.push({
      i,
      label: 'x'.repeat(32),
      nested: [i, i + 1, i + 2]
    });
  }
  return junk.length;
}

const parsed = JSON.parse('{"outer":{"items":[{"name":"a"},{"name":"b"}]}}', function (key, value) {
  gcPressure();
  if (key === 'name') return value.toUpperCase();
  return value;
});

assert(parsed.outer.items[0].name === 'A', 'reviver should preserve nested values under GC pressure');
assert(parsed.outer.items[1].name === 'B', 'reviver should preserve sibling values under GC pressure');

const source = {
  title: 'root',
  nested: {
    keep: 'ok',
    value: 7,
    toJSON() {
      gcPressure();
      return {
        keep: this.keep,
        value: this.value
      };
    }
  }
};

const json = JSON.stringify(source, function (key, value) {
  gcPressure();
  if (key === 'title') return value + '-done';
  return value;
});

const roundTrip = JSON.parse(json);
assert(roundTrip.title === 'root-done', 'replacer should preserve transformed root properties');
assert(roundTrip.nested.keep === 'ok', 'toJSON result should survive GC pressure');
assert(roundTrip.nested.value === 7, 'nested numeric values should survive GC pressure');

console.log('json temp roots stress: ok');
