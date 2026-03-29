let passed = 0;
let failed = 0;

function test(name, actual, expected) {
  if (actual === expected) {
    console.log(`  OK ${name}`);
    passed++;
    return;
  }

  console.log(`  FAIL ${name}: expected ${expected}, got ${actual}`);
  failed++;
}

async function returnsThenable() {
  return {
    then(resolve) {
      resolve(99);
    }
  };
}

console.log("Promise thenable adoption\n");

Promise.resolve({
  then(resolve) {
    resolve(42);
  }
}).then((value) => {
  test("Promise.resolve adopts plain thenables", value, 42);
});

returnsThenable().then((value) => {
  test("async return adopts plain thenables", value, 99);
});

setTimeout(() => {
  console.log(`\n${passed} passed, ${failed} failed`);
}, 20);
