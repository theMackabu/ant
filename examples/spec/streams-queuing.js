import { test, testThrows, summary } from './helpers.js';

console.log('CountQueuingStrategy / ByteLengthQueuingStrategy Tests\n');

const cqs = new CountQueuingStrategy({ highWaterMark: 5 });
test('CQS highWaterMark', cqs.highWaterMark, 5);
test('CQS size returns function', typeof cqs.size, 'function');
test('CQS size() returns 1', cqs.size(), 1);
test('CQS size(anything) returns 1', cqs.size({ byteLength: 1024 }), 1);

const cqs0 = new CountQueuingStrategy({ highWaterMark: 0 });
test('CQS highWaterMark 0', cqs0.highWaterMark, 0);

const cqs_large = new CountQueuingStrategy({ highWaterMark: 1024 });
test('CQS highWaterMark 1024', cqs_large.highWaterMark, 1024);

test('CQS size is same function each access', cqs.size === cqs.size, true);
test('CQS size is same across instances', cqs.size === cqs_large.size, true);
test('CQS size.name', cqs.size.name, 'size');
test('CQS size.length', cqs.size.length, 0);

testThrows('CQS requires new', () => CountQueuingStrategy({ highWaterMark: 1 }));
testThrows('CQS requires options', () => new CountQueuingStrategy());
testThrows('CQS requires highWaterMark', () => new CountQueuingStrategy({}));
testThrows('CQS rejects null', () => new CountQueuingStrategy(null));
testThrows('CQS rejects non-object', () => new CountQueuingStrategy(5));

test('CQS typeof', typeof CountQueuingStrategy, 'function');

const blqs = new ByteLengthQueuingStrategy({ highWaterMark: 1024 });
test('BLQS highWaterMark', blqs.highWaterMark, 1024);
test('BLQS size returns function', typeof blqs.size, 'function');

test('BLQS size with byteLength', blqs.size({ byteLength: 256 }), 256);
test('BLQS size with byteLength 0', blqs.size({ byteLength: 0 }), 0);
test('BLQS size with no byteLength', blqs.size({}), undefined);
test('BLQS size.name', blqs.size.name, 'size');
test('BLQS size.length', blqs.size.length, 1);

testThrows('BLQS size with undefined', () => blqs.size());
testThrows('BLQS size with null', () => blqs.size(null));

const blqs0 = new ByteLengthQueuingStrategy({ highWaterMark: 0 });
test('BLQS highWaterMark 0', blqs0.highWaterMark, 0);

test('BLQS size is same function each access', blqs.size === blqs.size, true);
test('BLQS size is same across instances', blqs.size === blqs0.size, true);

testThrows('BLQS requires new', () => ByteLengthQueuingStrategy({ highWaterMark: 1 }));
testThrows('BLQS requires options', () => new ByteLengthQueuingStrategy());
testThrows('BLQS requires highWaterMark', () => new ByteLengthQueuingStrategy({}));

test('BLQS typeof', typeof ByteLengthQueuingStrategy, 'function');
test('CQS size !== BLQS size', cqs.size !== blqs.size, true);

test('CQS toStringTag', Object.prototype.toString.call(cqs), '[object CountQueuingStrategy]');
test('BLQS toStringTag', Object.prototype.toString.call(blqs), '[object ByteLengthQueuingStrategy]');

const cqs_str = new CountQueuingStrategy({ highWaterMark: '10' });
test('CQS highWaterMark string coercion', cqs_str.highWaterMark, 10);

const blqs_str = new ByteLengthQueuingStrategy({ highWaterMark: '256' });
test('BLQS highWaterMark string coercion', blqs_str.highWaterMark, 256);

test('CQS highWaterMark false coercion', new CountQueuingStrategy({ highWaterMark: false }).highWaterMark, 0);
test('CQS highWaterMark true coercion', new CountQueuingStrategy({ highWaterMark: true }).highWaterMark, 1);

const cqs_frac = new CountQueuingStrategy({ highWaterMark: 2.5 });
test('CQS fractional highWaterMark', cqs_frac.highWaterMark, 2.5);

const buf = new Uint8Array([1, 2, 3, 4]);
test('BLQS size with Uint8Array', blqs.size(buf), 4);

testThrows('BLQS size propagates getter throw', () => {
  blqs.size({
    get byteLength() {
      throw new Error('boom');
    }
  });
});

summary();
