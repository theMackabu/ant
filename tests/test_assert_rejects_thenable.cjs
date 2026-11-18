const assert = require('node:assert');

const rejecting = { then(resolve, reject) { reject(new RangeError('thenable')); }, catch() {} };
const resolving = { then(resolve) { resolve(1); }, catch() {} };

(async () => {
  await assert.rejects(rejecting, RangeError);
  await assert.doesNotReject(resolving);
  console.log('ok');
})();
