import assert from 'node:assert';
import ops from 'ant:internal/shell_ops';

for (let i = 0; i < 10000; i++) {
  let failure;
  try {
    await ops.submit(null, null);
  } catch (error) {
    failure = error;
  }
  assert.ok(failure instanceof Error);
  assert.match(failure.message, /Invalid compiled process plan/);
}

console.log('shell ops rejected-promise GC stress passed');
