// Test Promise.all with resolved promises
async function test_promise_all_simple() {
  console.log('Testing Promise.all with simple values...');
  
  const promises = [
    Promise.resolve(1),
    Promise.resolve(2),
    Promise.resolve(3)
  ];
  
  const results = await Promise.all(promises);
  console.log('Results:', results);
}

// Test Promise.race with resolved promises
async function test_promise_race_simple() {
  console.log('Testing Promise.race...');
  
  const promises = [
    Promise.resolve('first'),
    Promise.resolve('second')
  ];
  
  const winner = await Promise.race(promises);
  console.log('Winner:', winner);
}

// Test fetch still works
async function test_fetch() {
  console.log('Testing fetch...');
  const response = await fetch('https://httpbin.org/get');
  console.log('Fetch status:', response.status);
}

void test_promise_all_simple();
console.log('---');
void test_promise_race_simple();
console.log('---');
void test_fetch();
