let pass = true;

const key = Symbol("k");
let coercions = 0;
let trappedKey = null;

const propKey = {
  [Symbol.toPrimitive](hint) {
    if (hint !== "string") {
      console.log("FAIL: property key @@toPrimitive should use string hint");
      pass = false;
    }
    coercions++;
    return key;
  }
};

const proxy = new Proxy({ [key]: 123 }, {
  get(target, actualKey, receiver) {
    trappedKey = actualKey;
    return Reflect.get(target, actualKey, receiver);
  }
});

if (proxy[propKey] !== 123) {
  console.log("FAIL: proxy get should use symbol returned from @@toPrimitive");
  pass = false;
}

if (trappedKey !== key) {
  console.log("FAIL: proxy get trap should receive symbol property key");
  pass = false;
}

if (coercions !== 1) {
  console.log("FAIL: property key @@toPrimitive should run exactly once");
  pass = false;
}

if (pass) console.log("PASS");
