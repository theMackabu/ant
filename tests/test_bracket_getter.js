class Test {
  constructor() {
    this._value = "initial";
  }

  get value() {
    console.log("getter called");
    return this._value;
  }
}

const t = new Test();
console.log("1. Get via dot:", t.value);
console.log("2. Get via bracket:", t["value"]);
