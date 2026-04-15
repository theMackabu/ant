let pass = true;

const visible = Symbol("visible");
const hidden = Symbol("hidden");
const getterKey = Symbol("getter");
let getterCalls = 0;

const source = {
  plain: 7,
  [visible]: 11,
};

Object.defineProperty(source, hidden, {
  value: 13,
  enumerable: false,
});

Object.defineProperty(source, getterKey, {
  enumerable: true,
  get() {
    getterCalls++;
    return 17;
  },
});

const copy = { ...source };

if (copy.plain !== 7) {
  console.log("FAIL: object spread should keep string-keyed data properties");
  pass = false;
}

if (copy[visible] !== 11) {
  console.log("FAIL: object spread should copy enumerable symbol properties");
  pass = false;
}

if (Object.getOwnPropertySymbols(copy).includes(hidden)) {
  console.log("FAIL: object spread should skip non-enumerable symbol properties");
  pass = false;
}

if (copy[getterKey] !== 17) {
  console.log("FAIL: object spread should read enumerable symbol getters");
  pass = false;
}

if (getterCalls !== 1) {
  console.log("FAIL: symbol getter should be invoked exactly once during spread");
  pass = false;
}

const getterDesc = Object.getOwnPropertyDescriptor(copy, getterKey);
if (!getterDesc || getterDesc.get) {
  console.log("FAIL: copied symbol getter should become a data property");
  pass = false;
}

if (!getterDesc || getterDesc.value !== 17) {
  console.log("FAIL: copied symbol getter value should be materialized");
  pass = false;
}

if (pass) console.log("PASS");
