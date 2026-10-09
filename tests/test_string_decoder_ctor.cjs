const assert = require('assert');
const { StringDecoder } = require('string_decoder');

const euro = Buffer.from('€');

const decoder = new StringDecoder('utf8');
assert.ok(decoder instanceof StringDecoder);
assert.strictEqual(decoder.write(euro.subarray(0, 1)), '');
assert.strictEqual(decoder.write(euro.subarray(1)), '€');
assert.strictEqual(decoder.end(), '');

assert.strictEqual(new StringDecoder('hex').write(Buffer.from([0xab, 0xcd])), 'abcd');

// iconv-lite style: initialize an existing object without new
function InternalDecoder(encoding) {
  StringDecoder.call(this, encoding);
}
InternalDecoder.prototype = StringDecoder.prototype;

const internal = new InternalDecoder('utf8');
assert.ok(internal instanceof StringDecoder);
assert.strictEqual(internal.write(euro.subarray(0, 2)), '');
assert.strictEqual(internal.end(euro.subarray(2)), '€');

const base64 = new InternalDecoder('base64');
assert.strictEqual(base64.write(Buffer.from('hi!')), 'aGkh');

console.log('string decoder ctor: ok');
