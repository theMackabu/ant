function assert(condition, message) {
  if (!condition) {
    console.log("FAIL: " + message);
    throw new Error(message);
  }
}

const ordinary = {
  [Symbol.toPrimitive]: null,
  valueOf() {
    return 7;
  },
};

const proxied = new Proxy({
  [Symbol.toPrimitive]: null,
  valueOf() {
    return 9;
  },
}, {});

const bad = {
  [Symbol.toPrimitive]: 1,
  valueOf() {
    return 11;
  },
};

assert(+ordinary === 7, "ordinary null @@toPrimitive should fall back");
assert(+proxied === 9, "proxy null @@toPrimitive should fall back");

let threw = false;
try {
  +bad;
} catch (e) {
  threw = e instanceof TypeError;
}

assert(threw, "non-null non-callable @@toPrimitive should throw");
console.log("OK");
