let passed = 0;
let failed = 0;

function test(name, actual, expected) {
  if (actual === expected) {
    console.log("PASS:", name);
    passed++;
    return;
  }
  console.log("FAIL:", name, "- expected", expected, "got", actual);
  failed++;
}

class Parent {
  constructor() {
    this.nodes = [1, 2, 3];
  }
  get names() {
    return this.nodes.reduce((sum, n) => sum + n, 0);
  }
}

class Child extends Parent {
  get names() {
    return super.names + 1;
  }
}

class Base {
  get value() {
    return this.tag;
  }
}

class Derived extends Base {
  constructor() {
    super();
    this.tag = "ok";
  }
  get value() {
    return super.value;
  }
}

test("super getter uses child receiver", new Child().names, 7);
test("super getter resolves child fields", new Derived().value, "ok");

console.log("Passed:", passed);
console.log("Failed:", failed);

if (failed > 0) throw new Error("test_super_getter_receiver failed");
