const assert = require("node:assert");

function assertArrayIncludesRegressionSurface() {
  assert.strictEqual([1, 2, 3].includes(2), true);
  assert.strictEqual([1, 2, 3].includes(4), false);
  assert.strictEqual([1, 2, 3].includes(1, 1), false);
  assert.strictEqual([NaN].includes(NaN), true);
  assert.strictEqual([1, 2, 3].includes(3, -1), true);
  assert.strictEqual([1, 2, 3].includes(1, -1), false);

  assert.strictEqual([,].includes(undefined), true);
  assert.strictEqual(Array(4).includes(undefined), true);
  assert.strictEqual(Array(4).includes(1), false);

  const holey = [];
  holey[3] = "tail";
  assert.strictEqual(holey.includes(undefined), true);
  assert.strictEqual(holey.includes("tail"), true);
  assert.strictEqual(holey.includes("tail", 4), false);

  const grown = [1];
  grown.length = 3;
  assert.strictEqual(grown.includes(undefined), true);

  let steps = 0;
  const generic = {
    get length() {
      steps += 1;
      return 6;
    },
    get 2() {
      steps += 10;
      return "match";
    },
    get 4() {
      steps += 100;
      return "late";
    }
  };
  assert.strictEqual([].includes.call(generic, "match", 1), true);
  assert.strictEqual(steps, 11);

  const proto = { 3: "from-proto" };
  const inherited = Object.create(proto);
  Object.defineProperty(inherited, "length", {
    configurable: true,
    enumerable: true,
    get() {
      return 5;
    }
  });
  assert.strictEqual([].includes.call(inherited, "from-proto"), true);
  assert.strictEqual([].includes.call(inherited, undefined), true);

  const proxyLog = [];
  const proxy = new Proxy(
    { length: 4, 0: NaN, 1: "", 2: NaN, 3: "" },
    {
      get(target, key) {
        proxyLog.push(String(key));
        return target[key];
      }
    }
  );
  assert.strictEqual(Array.prototype.includes.call(proxy, NaN, 1), true);
  assert.deepStrictEqual(proxyLog, ["length", "1", "2"]);

  [
    Int8Array,
    Uint8Array,
    Uint8ClampedArray,
    Int16Array,
    Uint16Array,
    Int32Array,
    Uint32Array,
    Float32Array,
    Float64Array
  ].forEach((TypedArray) => {
    const view = new TypedArray([1, 2, 3]);
    assert.strictEqual(view.includes(1), true);
    assert.strictEqual(view.includes(4), false);
    assert.strictEqual(view.includes(1, 1), false);
  });

  const floatView = new Float64Array([1, NaN, 3]);
  assert.strictEqual(floatView.includes(NaN), true);
}

assertArrayIncludesRegressionSurface();
console.log("array includes regression surface passes");
