// Array literal tests
let arr = [1, 2, 3];
console.log(arr);

// Array indexing
let first = arr[0];
let second = arr[1];
let third = arr[2];
console.log(first);
console.log(second);
console.log(third);

// Array length
console.log(arr.length);

// Empty array
let empty = [];
console.log(empty);
console.log(empty.length);

// Array with mixed types
let mixed = [1, "hello", true, null];
console.log(mixed);

// Array assignment
arr[0] = 10;
console.log(arr);

// Push method
arr.push(4);
console.log(arr);
console.log(arr.length);

arr.push(5, 6);
console.log(arr);
console.log(arr.length);

// Pop method
let popped = arr.pop();
console.log(popped);
console.log(arr);
console.log(arr.length);

// Nested arrays
let nested = [[1, 2], [3, 4]];
console.log(nested);
console.log(nested[0]);
console.log(nested[0][0]);
console.log(nested[1][1]);

// Array constructor
let arr2 = Array(3);
console.log(arr2);
console.log(arr2.length);

// Array constructor with elements
let arr3 = Array(10, 20, 30);
console.log(arr3);
console.log(arr3.length);

// Dynamic array creation
let dynamic = [];
dynamic[0] = "a";
dynamic[1] = "b";
dynamic[2] = "c";
console.log(dynamic);

// instanceof Array
console.log(arr instanceof Array);
console.log(mixed instanceof Array);
console.log({} instanceof Array);
console.log(5 instanceof Array);

// Bracket notation with string keys (object-like)
let obj = [1, 2, 3];
obj["foo"] = "bar";
console.log(obj.foo);

// Array iteration with for loop
let sum = 0;
for (let i = 0; i < arr.length; i = i + 1) {
    sum = sum + arr[i];
}
console.log(sum);

// Array of arrays
let matrix = [];
matrix[0] = [1, 2, 3];
matrix[1] = [4, 5, 6];
console.log(matrix);
console.log(matrix[0][1]);
console.log(matrix[1][2]);
