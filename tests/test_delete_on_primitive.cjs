const assert = require('node:assert');

class EdgeHolder { m() { return 1; } }
const edgeHolder = new EdgeHolder();
const edgeOps = {
  remove: v => delete v.x,
};
const edgeWarm = [edgeHolder, [1, 2], { x: 1, m() { return 2; }, 0: 5, length: 1 }, function () { return 3; }, 'str', EdgeHolder];
const edgeValues = [-Infinity, Infinity, NaN, -0, 1.5, -1e308, -1.7976931348623157e308];
const edgeExpected = {
  remove: v => true,
};
for (const [name, op] of Object.entries(edgeOps)) {
  for (let i = 0; i < 400; i++) { try { op(edgeWarm[i % edgeWarm.length]); } catch (e) {} }
  for (const v of edgeValues) assert.strictEqual(op(v), edgeExpected[name](v), `${name} on ${v}`);
}

console.log('ok');
