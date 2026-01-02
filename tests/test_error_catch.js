Promise.reject(new Error('Promise Test error')).catch(console.error);

try {
  throw new Error('try-catch test error');
} catch (err) {
  console.error(err);
}

async function testAsync() {
  throw new Error('Async test error');
}

testAsync().catch(console.error);
