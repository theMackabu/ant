function assert(condition, message) {
  if (!condition) {
    console.log("FAIL: " + message);
    throw new Error(message);
  }
}

globalThis.x = 1;
const events = [];
const target = {};
const proxy = new Proxy(target, {
  has(_target, key) {
    events.push("has:" + String(key));
    return key === "x";
  },
  set(_target, key, value) {
    events.push("set:" + String(key) + ":" + value);
    target[key] = value;
    return true;
  },
  deleteProperty(_target, key) {
    events.push("delete:" + String(key));
    delete target[key];
    return true;
  },
});

with (proxy) {
  x = 2;
}

assert(globalThis.x === 1, "proxy with assignment should not fall back to global");
assert(target.x === 2, "proxy with assignment should write through proxy");
assert(events.indexOf("has:x") >= 0, "proxy has trap should be used for assignment");
assert(events.indexOf("set:x:2") >= 0, "proxy set trap should be used for assignment");

const deleteResult = (function () {
  with (proxy) {
    return delete x;
  }
})();

assert(deleteResult === true, "proxy with delete should return delete result");
assert(!("x" in target), "proxy with delete should delete through proxy");
assert(events.indexOf("delete:x") >= 0, "proxy delete trap should be used");

const throwingProxy = new Proxy({}, {
  has(_target, key) {
    return key === "x";
  },
  deleteProperty() {
    throw new Error("delete boom");
  },
});

let propagated = false;
try {
  with (throwingProxy) {
    delete x;
  }
} catch (e) {
  propagated = e.message === "delete boom";
}

assert(propagated, "proxy with delete should propagate thrown delete errors");

const throwingSetProxy = new Proxy({}, {
  has(_target, key) {
    return key === "x";
  },
  set() {
    throw new Error("set boom");
  },
});

propagated = false;
try {
  with (throwingSetProxy) {
    x = 3;
  }
} catch (e) {
  propagated = e.message === "set boom";
}

assert(propagated, "proxy with assignment should propagate thrown set errors");

Object.defineProperty(globalThis, "withGlobalSetterThrow", {
  configurable: true,
  set() {
    throw new Error("global setter boom");
  },
});

propagated = false;
try {
  with ({}) {
    withGlobalSetterThrow = 1;
  }
} catch (e) {
  propagated = e.message === "global setter boom";
} finally {
  delete globalThis.withGlobalSetterThrow;
}

assert(propagated, "with global fallback assignment should propagate thrown setters");

console.log("OK");
