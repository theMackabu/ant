let passed = 0;
let failed = 0;

function test(name, actual, expected) {
  if (actual === expected) {
    console.log('PASS:', name);
    passed++;
  } else {
    console.log('FAIL:', name, '- expected', expected, 'got', actual);
    failed++;
  }
}

console.log('=== Static Properties ===');

class ClassWithStaticProperty {
  static staticProperty = "someValue";
}

test('static property access', ClassWithStaticProperty.staticProperty, "someValue");

console.log('\n=== Static Methods ===');

class ClassWithStaticMethod {
  static staticMethod() {
    return "static method called";
  }
}

test('static method call', ClassWithStaticMethod.staticMethod(), "static method called");

console.log('\n=== Static and Instance Together ===');

class Triple {
  static customName = "Tripler";
  static description = "I triple any number you provide";
  
  static calculate(n) {
    return n * 3;
  }
  
  constructor(value) {
    this.value = value;
  }
  
  getValue() {
    return this.value;
  }
}

test('static property customName', Triple.customName, "Tripler");
test('static property description', Triple.description, "I triple any number you provide");
test('static method calculate(1)', Triple.calculate(1), 3);
test('static method calculate(6)', Triple.calculate(6), 18);

let t = new Triple(42);
test('instance property', t.getValue(), 42);

console.log('\n=== Static Not on Instance ===');

test('static not on instance', t.customName, undefined);
test('static method not on instance', t.calculate, undefined);

console.log('\n=== Subclass with Static ===');

class SquaredTriple extends Triple {
  static description = "I square the triple of any number you provide";
  
  static calculate(n) {
    return super.calculate(n) * super.calculate(n);
  }
}

test('subclass static calculate(3)', SquaredTriple.calculate(3), 81);
test('subclass overrides static description', SquaredTriple.description, "I square the triple of any number you provide");

console.log('\n=== Static with this reference ===');

class StaticMethodCall {
  static staticProperty = "static property";
  
  static staticMethod() {
    return "Static method and " + this.staticProperty + " has been called";
  }
  
  static anotherStaticMethod() {
    return this.staticMethod() + " from another static method";
  }
}

test('static method using this.staticProperty', StaticMethodCall.staticMethod(), "Static method and static property has been called");
test('static method calling another static method', StaticMethodCall.anotherStaticMethod(), "Static method and static property has been called from another static method");

console.log('\n=== Static Field Without Initializer ===');

class ClassWithUndefinedStatic {
  static undefinedField;
  static definedField = "defined";
}

test('static field without initializer', ClassWithUndefinedStatic.undefinedField, undefined);
test('static field with initializer', ClassWithUndefinedStatic.definedField, "defined");

console.log('\n=== Static with Computed Values ===');

class MathHelper {
  static PI = 3.14159;
  static TWO_PI = 3.14159 * 2;
  
  static circleArea(r) {
    return this.PI * r * r;
  }
}

test('static computed value TWO_PI', MathHelper.TWO_PI, 3.14159 * 2);
test('static method using static property', MathHelper.circleArea(1), 3.14159);

console.log('\n=== Summary ===');
console.log('Passed:', passed);
console.log('Failed:', failed);
console.log('Total:', passed + failed);

if (failed > 0) {
  console.log('\nSome tests FAILED!');
} else {
  console.log('\nAll tests PASSED!');
}
