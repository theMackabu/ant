// Test: Automatic Semicolon Insertion (ASI)
// Tests that statements work correctly without explicit semicolons

console.log('=== ASI Tests ===')

// Test 1: const without semicolons
console.log('\nTest 1: const ASI')
const a = 1
const b = 2
const c = a + b
console.log('const a + b:', c)

// Test 2: let without semicolons
console.log('\nTest 2: let ASI')
let x = 10
let y = 20
let z = x * y
console.log('let x * y:', z)

// Test 3: else if without semicolons
console.log('\nTest 3: else if ASI')
let val = 50
if (val < 25) {
  console.log('less than 25')
} else if (val < 75) {
  console.log('between 25 and 75')
} else {
  console.log('75 or more')
}

// Test 4: Chained else if without semicolons
console.log('\nTest 4: Chained else if ASI')
let grade = 85
let letter
if (grade >= 90) {
  letter = 'A'
} else if (grade >= 80) {
  letter = 'B'
} else if (grade >= 70) {
  letter = 'C'
} else if (grade >= 60) {
  letter = 'D'
} else {
  letter = 'F'
}
console.log('Grade:', letter)

// Test 5: Mixed const/let without semicolons
console.log('\nTest 5: Mixed declarations ASI')
const PI = 3.14159
let radius = 5
let area = PI * radius * radius
console.log('Circle area:', area)

// Test 6: Dynamic import without semicolons (expression)
console.log('\nTest 6: Dynamic import ASI')
async function testImport() {
  const mod = await import('./export-test.js')
  mod.hello('asi')
}

// Test 7: Nested if/else if without semicolons
console.log('\nTest 7: Nested conditionals ASI')
let outer = true
let inner = false
if (outer) {
  if (inner) {
    console.log('both true')
  } else if (!inner) {
    console.log('outer true, inner false')
  }
} else if (!outer) {
  console.log('outer false')
}

// Test 8: Return statements without semicolons
console.log('\nTest 8: Return ASI')
function add(a, b) {
  return a + b
}
function multiply(a, b) {
  return a * b
}
console.log('add(3, 4):', add(3, 4))
console.log('multiply(3, 4):', multiply(3, 4))

// Test 9: Object/array literals with const/let
console.log('\nTest 9: Object/array ASI')
const obj = { name: 'test', value: 42 }
let arr = [1, 2, 3]
console.log('obj.name:', obj.name)
console.log('arr[1]:', arr[1])

// Test 10: Arrow functions without semicolons
console.log('\nTest 10: Arrow function ASI')
const double = x => x * 2
const triple = x => x * 3
console.log('double(5):', double(5))
console.log('triple(5):', triple(5))

// Test 11: for loop with let (ASI in body)
console.log('\nTest 11: for loop ASI')
let sum = 0
for (let i = 0; i < 5; i++) {
  sum = sum + i
}
console.log('sum:', sum)

// Test 12: while with else if pattern
console.log('\nTest 12: while + else if ASI')
let counter = 0
while (counter < 3) {
  if (counter === 0) {
    console.log('zero')
  } else if (counter === 1) {
    console.log('one')
  } else if (counter === 2) {
    console.log('two')
  }
  counter = counter + 1
}

// Run async test
testImport()

console.log('\n=== All ASI tests completed ===')
