import { test, testThrows, testDeep, summary } from './helpers.js';
import assert from 'node:assert';

console.log('assert.ok\n');

let threw = false;
try { assert(false); } catch (e) { threw = true; }
test('assert(false) throws', threw, true);

threw = false;
try { assert(0); } catch (e) { threw = true; }
test('assert(0) throws', threw, true);

threw = false;
try { assert(''); } catch (e) { threw = true; }
test('assert("") throws', threw, true);

threw = false;
try { assert(null); } catch (e) { threw = true; }
test('assert(null) throws', threw, true);

threw = false;
try { assert(1); } catch (e) { threw = true; }
test('assert(1) does not throw', threw, false);

threw = false;
try { assert('hello'); } catch (e) { threw = true; }
test('assert("hello") does not throw', threw, false);

threw = false;
try { assert.ok(false, 'custom message'); } catch (e) { threw = true; }
test('assert.ok with custom message throws', threw, true);

console.log('\nassert.equal / assert.notEqual\n');

threw = false;
try { assert.equal(1, 1); } catch (e) { threw = true; }
test('equal(1, 1) passes', threw, false);

threw = false;
try { assert.equal(1, '1'); } catch (e) { threw = true; }
test('equal(1, "1") passes (loose)', threw, false);

threw = false;
try { assert.equal(1, 2); } catch (e) { threw = true; }
test('equal(1, 2) throws', threw, true);

threw = false;
try { assert.equal(null, undefined); } catch (e) { threw = true; }
test('equal(null, undefined) passes (loose)', threw, false);

threw = false;
try { assert.notEqual(1, 2); } catch (e) { threw = true; }
test('notEqual(1, 2) passes', threw, false);

threw = false;
try { assert.notEqual(1, 1); } catch (e) { threw = true; }
test('notEqual(1, 1) throws', threw, true);

console.log('\nassert.strictEqual / assert.notStrictEqual\n');

threw = false;
try { assert.strictEqual(1, 1); } catch (e) { threw = true; }
test('strictEqual(1, 1) passes', threw, false);

threw = false;
try { assert.strictEqual('a', 'a'); } catch (e) { threw = true; }
test('strictEqual("a", "a") passes', threw, false);

threw = false;
try { assert.strictEqual(1, '1'); } catch (e) { threw = true; }
test('strictEqual(1, "1") throws', threw, true);

threw = false;
try { assert.strictEqual(null, undefined); } catch (e) { threw = true; }
test('strictEqual(null, undefined) throws', threw, true);

threw = false;
try { assert.strictEqual(NaN, NaN); } catch (e) { threw = true; }
test('strictEqual(NaN, NaN) passes', threw, false);

threw = false;
try { assert.notStrictEqual(1, 2); } catch (e) { threw = true; }
test('notStrictEqual(1, 2) passes', threw, false);

threw = false;
try { assert.notStrictEqual(1, 1); } catch (e) { threw = true; }
test('notStrictEqual(1, 1) throws', threw, true);

console.log('\nassert.deepEqual / assert.deepStrictEqual\n');

threw = false;
try { assert.deepEqual({ a: 1 }, { a: 1 }); } catch (e) { threw = true; }
test('deepEqual({a:1}, {a:1}) passes', threw, false);

threw = false;
try { assert.deepEqual([1, 2, 3], [1, 2, 3]); } catch (e) { threw = true; }
test('deepEqual arrays passes', threw, false);

threw = false;
try { assert.deepEqual({ a: 1 }, { a: 2 }); } catch (e) { threw = true; }
test('deepEqual({a:1}, {a:2}) throws', threw, true);

threw = false;
try { assert.deepEqual([1, 2], [1, 2, 3]); } catch (e) { threw = true; }
test('deepEqual different length arrays throws', threw, true);

threw = false;
try { assert.deepStrictEqual({ a: 1 }, { a: 1 }); } catch (e) { threw = true; }
test('deepStrictEqual({a:1}, {a:1}) passes', threw, false);

threw = false;
try { assert.deepStrictEqual({ a: 1 }, { a: '1' }); } catch (e) { threw = true; }
test('deepStrictEqual({a:1}, {a:"1"}) throws', threw, true);

threw = false;
try { assert.deepStrictEqual({ a: { b: { c: 42 } } }, { a: { b: { c: 42 } } }); } catch (e) { threw = true; }
test('deepStrictEqual nested objects passes', threw, false);

threw = false;
try { assert.notDeepEqual({ a: 1 }, { a: 2 }); } catch (e) { threw = true; }
test('notDeepEqual({a:1}, {a:2}) passes', threw, false);

threw = false;
try { assert.notDeepEqual({ a: 1 }, { a: 1 }); } catch (e) { threw = true; }
test('notDeepEqual({a:1}, {a:1}) throws', threw, true);

threw = false;
try { assert.notDeepStrictEqual({ a: 1 }, { a: '1' }); } catch (e) { threw = true; }
test('notDeepStrictEqual({a:1}, {a:"1"}) passes', threw, false);

console.log('\nassert.fail\n');

threw = false;
try { assert.fail('boom'); } catch (e) { threw = true; }
test('fail("boom") throws', threw, true);

threw = false;
try { assert.fail(); } catch (e) { threw = true; }
test('fail() throws', threw, true);

let failMsg = '';
try { assert.fail('specific message'); } catch (e) { failMsg = e.message; }
test('fail message is preserved', failMsg, 'specific message');

console.log('\nassert.ifError\n');

threw = false;
try { assert.ifError(null); } catch (e) { threw = true; }
test('ifError(null) does not throw', threw, false);

threw = false;
try { assert.ifError(undefined); } catch (e) { threw = true; }
test('ifError(undefined) does not throw', threw, false);

threw = false;
try { assert.ifError(new Error('oops')); } catch (e) { threw = true; }
test('ifError(Error) throws', threw, true);

threw = false;
try { assert.ifError('some error'); } catch (e) { threw = true; }
test('ifError(string) throws', threw, true);

console.log('\nassert.throws / assert.doesNotThrow\n');

threw = false;
try { assert.throws(() => { throw new Error('boom'); }); } catch (e) { threw = true; }
test('throws with throwing fn passes', threw, false);

threw = false;
try { assert.throws(() => {}); } catch (e) { threw = true; }
test('throws with non-throwing fn throws', threw, true);

threw = false;
try { assert.doesNotThrow(() => {}); } catch (e) { threw = true; }
test('doesNotThrow with non-throwing fn passes', threw, false);

threw = false;
try { assert.doesNotThrow(() => { throw new Error('oops'); }); } catch (e) { threw = true; }
test('doesNotThrow with throwing fn throws', threw, true);

console.log('\nassert.match / assert.doesNotMatch\n');

threw = false;
try { assert.match('hello world', /hello/); } catch (e) { threw = true; }
test('match passes when pattern matches', threw, false);

threw = false;
try { assert.match('hello world', /xyz/); } catch (e) { threw = true; }
test('match throws when pattern does not match', threw, true);

threw = false;
try { assert.doesNotMatch('hello world', /xyz/); } catch (e) { threw = true; }
test('doesNotMatch passes when pattern does not match', threw, false);

threw = false;
try { assert.doesNotMatch('hello world', /hello/); } catch (e) { threw = true; }
test('doesNotMatch throws when pattern matches', threw, true);

console.log('\nassert.rejects / assert.doesNotReject\n');

let resolved = false;
await assert.rejects(async () => { throw new Error('boom'); });
resolved = true;
test('rejects resolves when fn rejects', resolved, true);

resolved = false;
try { await assert.rejects(async () => {}); } catch (e) { resolved = false; }
test('rejects rejects when fn does not reject', resolved, false);

resolved = false;
await assert.doesNotReject(async () => {});
resolved = true;
test('doesNotReject resolves when fn does not reject', resolved, true);

console.log('\nAssertionError\n');

test('AssertionError is a constructor', typeof assert.AssertionError, 'function');

const ae = new assert.AssertionError({ message: 'test', actual: 1, expected: 2, operator: 'strictEqual' });
test('AssertionError has name', ae.name, 'AssertionError');
test('AssertionError has message', ae.message, 'test');
test('AssertionError has actual', ae.actual, 1);
test('AssertionError has expected', ae.expected, 2);
test('AssertionError has operator', ae.operator, 'strictEqual');

summary();
