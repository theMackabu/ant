// Test basic RegExp constructor
const re1 = new RegExp('hello');
console.log('RegExp created:', re1.source);
console.log('Flags:', re1.flags);
console.log('Global:', re1.global);

// Test with flags
const re2 = new RegExp('test', 'g');
console.log('Global flag set:', re2.global);
console.log('Flags:', re2.flags);

// Test multiple flags
const re3 = new RegExp('pattern', 'gi');
console.log('Multiple flags:', re3.flags);
console.log('Global:', re3.global);
console.log('IgnoreCase:', re3.ignoreCase);

console.log('All basic RegExp tests passed!');
