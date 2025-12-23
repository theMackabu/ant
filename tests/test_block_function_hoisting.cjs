// Test function declaration hoisting inside blocks
// Function declarations should be hoisted to the top of their containing block

console.log("=== Test 1: Function declaration hoisted in regular block ===");
{
    foo();
    function foo() {
        console.log("foo called - hoisted in block");
    }
}

console.log("\n=== Test 2: Function hoisting in arrow function body ===");
const test2 = () => {
    bar();
    function bar() {
        console.log("bar called - hoisted in arrow function");
    }
};
test2();

console.log("\n=== Test 3: Function hoisting in callback ===");
const arr = [1];
arr.forEach((x) => {
    baz();
    function baz() {
        console.log("baz called with", x, "- hoisted in callback");
    }
});

console.log("\n=== Test 4: Function hoisting in .map() callback ===");
const result = [5].map((x) => {
    return helper(x);
    function helper(n) {
        return n * 2;
    }
});
console.log("result should be [10]:", result);

console.log("\n=== Test 5: Nested function calling before definition ===");
function outer() {
    inner();
    function inner() {
        console.log("inner called - hoisted inside outer");
    }
}
outer();

console.log("\n=== Test 6: Function modifying outer variable before definition ===");
function test6() {
    let value = 0;
    modify();
    console.log("value after modify should be 42:", value);
    
    function modify() {
        value = 42;
    }
}
test6();

console.log("\n=== All block function hoisting tests completed ===");
