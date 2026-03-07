let getterCalls = 0;

Object.defineProperty(module.exports, "answer", {
  enumerable: true,
  get() {
    getterCalls += 1;
    return 42;
  },
});

Object.defineProperty(module.exports, "getterCalls", {
  enumerable: true,
  get() {
    return getterCalls;
  },
});
