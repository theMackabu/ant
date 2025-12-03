// Test String methods
console.log('String methods:');
console.log('"hello".toUpperCase() =', "hello".toUpperCase());
console.log('"HELLO".toLowerCase() =', "HELLO".toLowerCase());
console.log('"  hello  ".trim() =', "  hello  ".trim());
console.log('"abc".repeat(3) =', "abc".repeat(3));
console.log('"5".padStart(3, "0") =', "5".padStart(3, "0"));
console.log('"5".padEnd(3, "0") =', "5".padEnd(3, "0"));
console.log('"hello".charAt(1) =', "hello".charAt(1));

// Test Number methods
console.log('\nNumber methods:');
console.log('(42).toString() =', (42).toString());
console.log('(3.14159).toFixed(2) =', (3.14159).toFixed(2));
console.log('(123.456).toPrecision(4) =', (123.456).toPrecision(4));
console.log('(1234).toExponential(2) =', (1234).toExponential(2));

// Test parseInt and parseFloat
console.log('\nparseInt and parseFloat:');
console.log('parseInt("42") =', parseInt("42"));
console.log('parseInt("ff", 16) =', parseInt("ff", 16));
console.log('parseInt("1010", 2) =', parseInt("1010", 2));
console.log('parseFloat("3.14") =', parseFloat("3.14"));
console.log('parseFloat("3.14e2") =', parseFloat("3.14e2"));
