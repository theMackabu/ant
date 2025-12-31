class Test {
  constructor() {
    this._value = "initial";
  }

  get value() {
    return this._value;
  }

  set value(v) {
    console.log("setter called with:", v);
    this._value = v;
  }
}

const t = new Test();
console.log("1. Get via dot:", t.value);
t.value = "changed via dot";
console.log("2. After dot set:", t.value);

console.log("3. Get via bracket:", t["value"]);
t["value"] = "changed via bracket";
console.log("4. After bracket set:", t["value"]);
