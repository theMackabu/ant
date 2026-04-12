let pass = true;

const key = Symbol("reflect-key");
let seenHint = null;
let trappedKey = null;

const propertyKeyObject = {
  [Symbol.toPrimitive](hint) {
    seenHint = hint;
    return key;
  }
};

const proxy = new Proxy({ [key]: 42 }, {
  get(target, actualKey, receiver) {
    trappedKey = actualKey;
    return Reflect.get(target, actualKey, receiver);
  }
});

const value = Reflect.get(proxy, propertyKeyObject);
if (value !== 42) {
  console.log("FAIL: Reflect.get should read using the property key from @@toPrimitive");
  pass = false;
}

if (seenHint !== "string") {
  console.log("FAIL: Reflect.get property key coercion should use string hint");
  pass = false;
}

if (trappedKey !== key) {
  console.log("FAIL: proxy get trap should receive the symbol returned by @@toPrimitive");
  pass = false;
}

if (pass) console.log("PASS");
