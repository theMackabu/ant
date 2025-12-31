const obj = {};

Object.defineProperty(obj, 'value', {
  get() {
    console.log("getter called");
    return this._value;
  },
  set(v) {
    console.log("setter called with:", v);
    this._value = v;
  }
});

obj._value = "initial";
console.log("1. Get:", obj.value);
obj.value = "changed";
console.log("2. Get after set:", obj.value);
