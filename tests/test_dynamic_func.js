const add = new Function("a", "b", "return a + b");
console.log("add:", add(2, 3));
console.log("add:", add(10, 20));

const greet = new Function("name", "return \"Hello, \" + name + \"!\"");
console.log("greet:", greet("World"));

const noArgs = new Function("return 42");
console.log("noArgs:", noArgs());

const empty = new Function();
console.log("empty:", empty());

console.log("toString:", add.toString());
console.log("done");
