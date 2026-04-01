// Repro: object spread should invoke getters

const src = {};
Object.defineProperty(src, 'x', {
  get() { return 42; },
  enumerable: true,
});
Object.defineProperty(src, 'y', {
  get() { return 'hello'; },
  enumerable: true,
});
src.z = 99; // plain data property for comparison

const copy = { ...src };

let pass = true;

if (copy.x !== 42) {
  console.log("FAIL: copy.x expected 42, got", copy.x);
  pass = false;
}
if (copy.y !== 'hello') {
  console.log("FAIL: copy.y expected 'hello', got", copy.y);
  pass = false;
}
if (copy.z !== 99) {
  console.log("FAIL: copy.z expected 99, got", copy.z);
  pass = false;
}

// Also test getter that references `this`
const src2 = { _val: 10 };
Object.defineProperty(src2, 'doubled', {
  get() { return this._val * 2; },
  enumerable: true,
});

const copy2 = { ...src2 };
if (copy2.doubled !== 20) {
  console.log("FAIL: copy2.doubled expected 20, got", copy2.doubled);
  pass = false;
}
if (copy2._val !== 10) {
  console.log("FAIL: copy2._val expected 10, got", copy2._val);
  pass = false;
}

// Getter-only (no setter) should still copy the value, not the accessor
const desc = Object.getOwnPropertyDescriptor(copy, 'x');
if (desc.get) {
  console.log("FAIL: copy.x should be a data property, not an accessor");
  pass = false;
}
if (desc.value !== 42) {
  console.log("FAIL: copy.x descriptor value expected 42, got", desc.value);
  pass = false;
}

if (pass) console.log("PASS");
