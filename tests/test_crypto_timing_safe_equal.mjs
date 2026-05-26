import assert from "node:assert";
import { timingSafeEqual } from "node:crypto";

const left = new Uint8Array([1, 2, 3, 4]);
const same = new Uint8Array([1, 2, 3, 4]);
const different = new Uint8Array([1, 2, 3, 5]);

assert.equal(typeof timingSafeEqual, "function");
assert.equal(timingSafeEqual(left, same), true);
assert.equal(timingSafeEqual(left, different), false);

assert.equal(typeof crypto.subtle.timingSafeEqual, "function");
assert.equal(crypto.subtle.timingSafeEqual(new DataView(left.buffer), same), true);

let caught = null;
try {
  timingSafeEqual(new Uint8Array([1]), new Uint8Array([1, 2]));
} catch (err) {
  caught = err;
}

assert.equal(caught?.name, "RangeError");

console.log("crypto timingSafeEqual test passed");
