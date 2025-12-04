console.log('Testing recursive string assignment with objects');

function testRecursion(depth, params) {
  if (depth >= 10) return 'done';

  let paramValue = '';
  // This assignment causes parser corruption in certain contexts
  paramValue = 'test';

  if (paramValue !== '') {
    params['test' + depth] = paramValue;
    const result = testRecursion(depth + 1, params);
    delete params['test' + depth];
    return result;
  }

  return 'failed';
}

const params = {};
const result = testRecursion(0, params);
console.log('Result:', result);
