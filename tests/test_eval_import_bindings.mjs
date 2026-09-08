import assert, { strictEqual as equal } from 'node:assert';
import * as assertions from 'node:assert';

function importedEvalBindings(source) {
  equal(typeof assert, 'function');
  equal(typeof equal, 'function');
  equal(typeof assertions.strictEqual, 'function');
  eval(source);
  equal(eval('typeof equal'), 'function');
  assert.ok(true);
  assertions.strictEqual(1, 1);
}
importedEvalBindings('0');
console.log('eval import binding tests passed');
