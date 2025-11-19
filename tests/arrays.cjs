// Array literal tests
let arr = [1, 2, 3];
Ant.println(arr);

// Array indexing
let first = arr[0];
let second = arr[1];
let third = arr[2];
Ant.println(first);
Ant.println(second);
Ant.println(third);

// Array length
Ant.println(arr.length);

// Empty array
let empty = [];
Ant.println(empty);
Ant.println(empty.length);

// Array with mixed types
let mixed = [1, "hello", true, null];
Ant.println(mixed);

// Array assignment
arr[0] = 10;
Ant.println(arr);

// Push method
arr.push(4);
Ant.println(arr);
Ant.println(arr.length);

arr.push(5, 6);
Ant.println(arr);
Ant.println(arr.length);

// Pop method
let popped = arr.pop();
Ant.println(popped);
Ant.println(arr);
Ant.println(arr.length);

// Nested arrays
let nested = [[1, 2], [3, 4]];
Ant.println(nested);
Ant.println(nested[0]);
Ant.println(nested[0][0]);
Ant.println(nested[1][1]);

// Array constructor
let arr2 = Array(3);
Ant.println(arr2);
Ant.println(arr2.length);

// Array constructor with elements
let arr3 = Array(10, 20, 30);
Ant.println(arr3);
Ant.println(arr3.length);

// Dynamic array creation
let dynamic = [];
dynamic[0] = "a";
dynamic[1] = "b";
dynamic[2] = "c";
Ant.println(dynamic);

// instanceof Array
Ant.println(arr instanceof Array);
Ant.println(mixed instanceof Array);
Ant.println({} instanceof Array);
Ant.println(5 instanceof Array);

// Bracket notation with string keys (object-like)
let obj = [1, 2, 3];
obj["foo"] = "bar";
Ant.println(obj.foo);

// Array iteration with for loop
let sum = 0;
for (let i = 0; i < arr.length; i = i + 1) {
    sum = sum + arr[i];
}
Ant.println(sum);

// Array of arrays
let matrix = [];
matrix[0] = [1, 2, 3];
matrix[1] = [4, 5, 6];
Ant.println(matrix);
Ant.println(matrix[0][1]);
Ant.println(matrix[1][2]);
