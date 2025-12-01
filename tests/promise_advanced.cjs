// Test Promise.all with multiple concurrent fetches
async function test_promise_all() {
  console.log('Testing Promise.all with concurrent fetches...');

  const promises = [fetch('https://httpbin.org/delay/1'), fetch('https://httpbin.org/get'), fetch('https://httpbin.org/user-agent')];

  const responses = await Promise.all(promises);
  console.log('All requests completed!');
  console.log('Response count:', responses.length);
}

// Test Promise.race
async function test_promise_race() {
  console.log('Testing Promise.race...');

  const promises = [fetch('https://httpbin.org/delay/2'), fetch('https://httpbin.org/get'), fetch('https://httpbin.org/delay/3')];

  const winner = await Promise.race(promises);
  console.log('Race winner status:', winner.status);
}

// Test with timers and promises
async function test_timer_integration() {
  console.log('Testing timer integration with await...');

  const start = Date.now();
  await new Promise(resolve => setTimeout(resolve, 100));

  const elapsed = Date.now() - start;
  console.log('Timer completed in', elapsed, 'ms');
}

// Run tests
void test_promise_all();
console.log('---');
void test_promise_race();
console.log('---');
void test_timer_integration();
