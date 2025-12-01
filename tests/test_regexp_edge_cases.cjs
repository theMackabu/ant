// Test edge cases for RegExp

// Empty pattern
const re1 = new RegExp('');
console.log('Empty pattern source:', re1.source);

// Empty string replacement
const str1 = 'hello';
const result1 = str1.replace(new RegExp('l', 'g'), '');
console.log('Remove chars:', result1);

// No match
const str2 = 'hello world';
const result2 = str2.replace(new RegExp('xyz'), 'foo');
console.log('No match:', result2);

// Special characters that need escaping
const str3 = 'Price: $10.99';
const re3 = new RegExp('[0-9.]+');
const result3 = str3.replace(re3, '20.00');
console.log('Price replace:', result3);

// Whitespace replacement
const str4 = 'hello   world';
const re4 = new RegExp(' +', 'g');
const result4 = str4.replace(re4, ' ');
console.log('Normalize spaces:', result4);

// Replace at start
const str5 = 'hello world';
const re5 = new RegExp('^hello');
const result5 = str5.replace(re5, 'hi');
console.log('Replace at start:', result5);

// Replace at end  
const str6 = 'hello world';
const re6 = new RegExp('world$');
const result6 = str6.replace(re6, 'there');
console.log('Replace at end:', result6);

console.log('All edge case tests passed!');
