// Test basic generator
function* simpleGenerator() {
  yield 1;
  yield 2;
  yield 3;
}

const gen = simpleGenerator();
console.log("Test 1: Basic generator");
console.log(gen.next().value); // 1
console.log(gen.next().value); // 2
console.log(gen.next().value); // 3
console.log(gen.next().done);  // true

// Test generator with parameters
console.log("\nTest 2: Generator with parameters");
function* foo(index) {
  while (index < 2) {
    yield index;
    index++;
  }
}

const iterator = foo(0);
console.log(iterator.next().value); // 0
console.log(iterator.next().value); // 1
console.log(iterator.next().done);  // true

// Test generator with done property
console.log("\nTest 3: Generator done property");
function* countAppleSales() {
  const saleList = [3, 7, 5];
  let i = 0;
  while (i < 3) {
    yield saleList[i];
    i++;
  }
}

const appleStore = countAppleSales();
let result = appleStore.next();
console.log(result.value, result.done); // 3 false
result = appleStore.next();
console.log(result.value, result.done); // 7 false
result = appleStore.next();
console.log(result.value, result.done); // 5 false
result = appleStore.next();
console.log(result.value, result.done); // undefined true

// Test infinite generator
console.log("\nTest 4: Infinite generator");
function* infinite() {
  let index = 0;
  while (true) {
    yield index++;
  }
}

const infGen = infinite();
console.log(infGen.next().value); // 0
console.log(infGen.next().value); // 1
console.log(infGen.next().value); // 2
