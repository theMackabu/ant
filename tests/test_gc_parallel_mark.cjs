const assert = require('node:assert');

const sym = Symbol('slot');
class Point {
  constructor(x, y) { this.x = x; this.y = y; }
  get sum() { return this.x + this.y; }
}

function makeNode(i) {
  const tag = 'node-' + i;
  const node = {
    id: i,
    label: tag + (i % 3 === 0 ? '-ü' : '') + '-' + 'x'.repeat(i % 50),
    rope: 'r'.repeat(40 + (i % 30)) + tag,
    list: [i, tag, i % 2 ? null : { nested: i }, , i * 2],
    point: new Point(i, -i),
    fn: (k => () => k + i)(i % 7),
    big: BigInt(i) * 1000000007n,
    [sym]: i,
  };
  if (i % 11 === 0) node.map = new Map([[tag, { v: i }], [i, [tag]]]);
  if (i % 13 === 0) node.set = new Set([tag, i, node.list]);
  if (i % 17 === 0) node.bytes = new Uint8Array([i & 255, (i >> 8) & 255]);
  return node;
}

function checkNode(node, i) {
  const tag = 'node-' + i;
  assert.strictEqual(node.id, i);
  assert.strictEqual(node.label, tag + (i % 3 === 0 ? '-ü' : '') + '-' + 'x'.repeat(i % 50));
  assert.strictEqual(node.rope, 'r'.repeat(40 + (i % 30)) + tag);
  assert.strictEqual(node.list[1], tag);
  assert.strictEqual(node.list.length, 5);
  assert.strictEqual(3 in node.list, false);
  if (i % 2 === 0) assert.strictEqual(node.list[2].nested, i);
  assert.strictEqual(node.point.sum, 0);
  assert.strictEqual(node.fn(), (i % 7) + i);
  assert.strictEqual(node.big, BigInt(i) * 1000000007n);
  assert.strictEqual(node[sym], i);
  if (i % 11 === 0) {
    assert.strictEqual(node.map.get(tag).v, i);
    assert.strictEqual(node.map.get(i)[0], tag);
  }
  if (i % 13 === 0) assert.ok(node.set.has(tag) && node.set.has(node.list));
  if (i % 17 === 0) assert.strictEqual(node.bytes[0], i & 255);
}

const COUNT = 30000;
const root = [];
for (let i = 0; i < COUNT; i++) {
  const node = makeNode(i);
  if (i > 0) node.prev = root[i - 1];
  root.push(node);
}

let sink = 0;
for (let round = 0; round < 12; round++) {
  for (let j = 0; j < 150000; j++) {
    const garbage = { a: j, b: [j, j + 1], c: 'g' + j };
    sink += garbage.b.length;
  }
  for (let i = round; i < COUNT; i += 97) root[i] = makeNode(i);
  for (let i = 1; i < COUNT; i += 1) root[i].prev = root[i - 1];
}

for (let i = 0; i < COUNT; i++) {
  checkNode(root[i], i);
  if (i > 0) assert.strictEqual(root[i].prev, root[i - 1]);
}
assert.ok(sink > 0);
console.log('ok');
