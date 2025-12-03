import { $ } from 'ant/shell';

console.log('Testing $ shell command execution...');

console.log('\n=== Test 1: Simple echo ===');
const result1 = await $`echo "Hello, world!"`;
const text1 = result1.text();
console.log('Output:', text1);
if (text1 !== 'Hello, world!') {
  throw new Error('Expected "Hello, world!" but got: ' + text1);
}
if (result1.exitCode !== 0) {
  throw new Error('Expected exit code 0 but got: ' + result1.exitCode);
}

console.log('\n=== Test 2: Multi-line output ===');
const result2 = await $`echo "line1\nline2\nline3"`;
const lines2 = result2.lines();
console.log('Line count:', lines2.length);
console.log('Line 0:', lines2[0]);
console.log('Line 1:', lines2[1]);
console.log('Line 2:', lines2[2]);

if (lines2.length !== 3) {
  throw new Error('Expected 3 lines but got: ' + lines2.length);
}
if (lines2[0] !== 'line1') {
  throw new Error('Expected "line1" but got: ' + lines2[0]);
}
if (lines2[1] !== 'line2') {
  throw new Error('Expected "line2" but got: ' + lines2[1]);
}
if (lines2[2] !== 'line3') {
  throw new Error('Expected "line3" but got: ' + lines2[2]);
}

console.log('\n=== Test 3: ls command ===');
const result3 = await $`ls tests/test_shell_dollar.cjs`;
const text3 = result3.text();
console.log('Output:', text3);
if (!text3.includes('test_shell_dollar.cjs')) {
  throw new Error('Expected output to contain test_shell_dollar.cjs');
}

console.log('\n=== Test 4: pwd command ===');
const result4 = await $`pwd`;
const text4 = result4.text();
console.log('Current directory:', text4);
if (text4.length === 0) {
  throw new Error('Expected non-empty pwd output');
}

console.log('\n=== Test 5: .text() method ===');
const result5 = await $`echo "test text method"`;
const text5 = result5.text();
console.log('Text:', text5);
if (text5 !== 'test text method') {
  throw new Error('Expected "test text method" but got: ' + text5);
}

console.log('\n=== Test 6: Listing files with .lines() ===');
const result6 = await $`ls tests/*.cjs | head -5`;
const lines6 = result6.lines();
console.log('Found', lines6.length, 'files');
for (let i = 0; i < lines6.length && i < 3; i++) {
  console.log('  -', lines6[i]);
}

console.log('\n=== Test 7: date command ===');
const result7 = await $`date +%Y`;
const year = result7.text();
console.log('Current year:', year);
if (year.length !== 4) {
  throw new Error('Expected 4-digit year');
}

console.log('\n✓ All $ shell tests passed!');
