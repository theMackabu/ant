// Test method with 3 parameters where last 2 have defaults
class Formatter {
    wrap(text, left = '(', right = ')') {
        console.log('wrap called with:', text, left, right);
        return left + text + right;
    }
}

console.log("Test 1 - All 3 params provided:");
const fmt = new Formatter();
const result1 = fmt.wrap('test', '<', '>');
console.log('Result:', result1);

console.log("\nTest 2 - Only first param (2 defaults):");
const result2 = fmt.wrap('test');
console.log('Result:', result2);

console.log("\nTest 3 - First 2 params (1 default):");
const result3 = fmt.wrap('test', '[');
console.log('Result:', result3);
