// Test Object.prototype.toString

console.log("Testing Object.prototype.toString:");

// Test with Atomics (has @@toStringTag)
console.log("Object.prototype.toString.call(Atomics):", Object.prototype.toString.call(Atomics));

// Test with other built-ins
console.log("\nOther types:");
console.log("Object.prototype.toString.call({}):", Object.prototype.toString.call({}));
console.log("Object.prototype.toString.call([]):", Object.prototype.toString.call([]));
console.log("Object.prototype.toString.call(function(){}):", Object.prototype.toString.call(function(){}));
console.log("Object.prototype.toString.call(42):", Object.prototype.toString.call(42));
console.log("Object.prototype.toString.call('hello'):", Object.prototype.toString.call("hello"));
console.log("Object.prototype.toString.call(true):", Object.prototype.toString.call(true));
console.log("Object.prototype.toString.call(null):", Object.prototype.toString.call(null));
console.log("Object.prototype.toString.call(undefined):", Object.prototype.toString.call(undefined));

// Test with custom @@toStringTag
console.log("\nCustom @@toStringTag:");
const custom = { "@@toStringTag": "MyCustomType" };
console.log("Object.prototype.toString.call(custom):", Object.prototype.toString.call(custom));
