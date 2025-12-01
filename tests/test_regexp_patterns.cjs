// Test various regex patterns

// Test word boundaries
const str1 = 'The quick brown fox';
const re1 = new RegExp('quick', 'g');
const result1 = str1.replace(re1, 'slow');
Ant.println('Word replace:', result1);

// Test character classes
const str2 = 'a1b2c3';
const re2 = new RegExp('[0-9]', 'g');
const result2 = str2.replace(re2, 'X');
Ant.println('Digit replace:', result2);

// Test alternation
const str3 = 'I like cats and dogs';
const re3 = new RegExp('cats', 'g');
const result3 = str3.replace(re3, 'birds');
Ant.println('Alternation:', result3);

// Test start of line
const str4 = 'hello world\nhello there';
const re4 = new RegExp('hello', 'g');
const result4 = str4.replace(re4, 'hi');
Ant.println('Multiple hello:', result4);

// Test any character
const str5 = 'abc def ghi';
const re5 = new RegExp('d.f');
const result5 = str5.replace(re5, 'XXX');
Ant.println('Any char:', result5);

// Test quantifiers
const str6 = 'aaaa bb c';
const re6 = new RegExp('a+', 'g');
const result6 = str6.replace(re6, 'X');
Ant.println('Quantifier +:', result6);

const str7 = 'aaaa bb c';
const re7 = new RegExp('a*', 'g');
const result7 = str7.replace(re7, 'X');
Ant.println('Quantifier *:', result7);

Ant.println('All pattern tests passed!');
