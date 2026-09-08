const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

for (const count of [1023, 1024, 1025, 1050]) {
  const source = `
function manyLabels(value) {
  let result=0;
  ${'if(value)result++;'.repeat(count)}
  return result;
}
for(let i=0;i<150;i++) {
  if(manyLabels(true)!==${count}) throw new Error('true result');
  if(manyLabels(false)!==0) throw new Error('false result');
}
function following(value) { return value+1; }
for(let i=0;i<200;i++) {
  if(following(i)!==i+1) throw new Error('subsequent compilation');
}
console.log('labels ok');
`;
  const child = spawnSync(process.execPath, ['-e', source], {
    encoding: 'utf8',
    env: { ...process.env, ANT_DEBUG: '' },
    timeout: 10000,
  });
  assert.strictEqual(child.status, 0, `label count ${count}: status ${child.status}, signal ${child.signal}`);
  assert.match(child.stdout, /labels ok/);
}
console.log('jit label capacity: ok');
