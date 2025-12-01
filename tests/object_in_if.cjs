// Test objects in if statements
// Tests property access and truthiness evaluation

console.log("=== Object in If Statements Test ===\n");

// Test 1: Basic object property truthiness
console.log("Test 1: Object property truthiness");
let thing = {
  hasThing: true,
  hasOther: false,
  count: 5,
  name: "test"
};

if (thing.hasThing) {
  console.log("  thing.hasThing is truthy: PASS");
}

if (!thing.hasOther) {
  console.log("  !thing.hasOther is falsy: PASS");
}

if (thing.count) {
  console.log("  thing.count (5) is truthy: PASS");
}

if (thing.name) {
  console.log("  thing.name ('test') is truthy: PASS");
}

// Test 2: Undefined and null properties
console.log("\nTest 2: Undefined and null properties");
let obj = {
  defined: "value",
  nullValue: null,
  undefinedValue: undefined,
  zeroValue: 0,
  emptyString: ""
};

if (!obj.nonExistent) {
  console.log("  !obj.nonExistent (undefined): PASS");
}

if (!obj.nullValue) {
  console.log("  !obj.nullValue (null): PASS");
}

if (!obj.undefinedValue) {
  console.log("  !obj.undefinedValue (undefined): PASS");
}

if (!obj.zeroValue) {
  console.log("  !obj.zeroValue (0): PASS");
}

if (!obj.emptyString) {
  console.log("  !obj.emptyString (''): PASS");
}

if (obj.defined) {
  console.log("  obj.defined ('value'): PASS");
}

// Test 3: Nested object properties
console.log("\nTest 3: Nested object properties");
let config = {
  settings: {
    enabled: true,
    disabled: false,
    nested: {
      deep: "value"
    }
  }
};

if (config.settings) {
  console.log("  config.settings exists: PASS");
}

if (config.settings.enabled) {
  console.log("  config.settings.enabled is true: PASS");
}

if (!config.settings.disabled) {
  console.log("  !config.settings.disabled is false: PASS");
}

if (config.settings.nested) {
  console.log("  config.settings.nested exists: PASS");
}

if (config.settings.nested.deep) {
  console.log("  config.settings.nested.deep has value: PASS");
}

// Test 4: Function properties
console.log("\nTest 4: Function properties");
let api = {
  hasMethod: function() {
    return "called";
  },
  noMethod: null
};

if (api.hasMethod) {
  console.log("  api.hasMethod exists:", api.hasMethod());
}

if (!api.noMethod) {
  console.log("  !api.noMethod is null: PASS");
}

// Test 5: Array properties
console.log("\nTest 5: Array properties");
let data = {
  items: [1, 2, 3],
  emptyItems: [],
  noItems: null
};

if (data.items) {
  console.log("  data.items exists, length:", data.items.length);
}

if (data.emptyItems) {
  console.log("  data.emptyItems exists but empty, length:", data.emptyItems.length);
}

if (!data.noItems) {
  console.log("  !data.noItems is null: PASS");
}

// Test 6: Conditional checks with property access
console.log("\nTest 6: Conditional property checks");
let user = {
  name: "John",
  age: 30,
  active: true
};

if (user.name && user.age) {
  console.log("  user.name && user.age both exist: PASS");
}

if (user.active && user.name) {
  console.log("  user.active && user.name both truthy: PASS");
}

if (!user.deleted || user.active) {
  console.log("  !user.deleted || user.active: PASS");
}

// Test 7: Return based on object property
console.log("\nTest 7: Return based on property");
function checkUser(user) {
  if (!user) return { error: "no user" };
  if (!user.name) return { error: "no name" };
  return { success: true, name: user.name };
}

console.log("  With null:", checkUser(null));
console.log("  With no name:", checkUser({ age: 25 }));
console.log("  With name:", checkUser({ name: "Alice" }));

// Test 8: Object itself as condition
console.log("\nTest 8: Object as condition");
let obj1 = { value: 1 };
let obj2 = null;
let obj3 = undefined;

if (obj1) {
  console.log("  obj1 (object) is truthy: PASS");
}

if (!obj2) {
  console.log("  !obj2 (null) is falsy: PASS");
}

if (!obj3) {
  console.log("  !obj3 (undefined) is falsy: PASS");
}

// Test 9: Boolean properties in complex conditions
console.log("\nTest 9: Complex boolean property checks");
let feature = {
  enabled: true,
  experimental: false,
  beta: true,
  stable: false
};

if (feature.enabled && feature.beta) {
  console.log("  enabled && beta: PASS");
}

if (feature.enabled && !feature.stable) {
  console.log("  enabled && !stable: PASS");
}

if (!feature.experimental && !feature.stable) {
  console.log("  !experimental && !stable: PASS");
}

// Test 10: Property chain with guard
console.log("\nTest 10: Safe property access");
function getValue(obj) {
  if (!obj) return "no object";
  if (!obj.data) return "no data";
  if (!obj.data.value) return "no value";
  return obj.data.value;
}

console.log("  With null:", getValue(null));
console.log("  With no data:", getValue({}));
console.log("  With no value:", getValue({ data: {} }));
console.log("  With value:", getValue({ data: { value: "found" } }));

// Test 11: Optional chaining with undefined/null values
console.log("\nTest 11: Optional chaining (?.)");
const value = undefined;
const nullValue = null;
const obj11 = { nested: { deep: "value" } };

console.log("  value?.thing (undefined):", value?.thing);
console.log("  nullValue?.thing (null):", nullValue?.thing);
console.log("  obj11?.nested?.deep:", obj11?.nested?.deep);
console.log("  obj11?.missing?.deep:", obj11?.missing?.deep);

if (value?.thing) {
  console.log("    FAIL: value?.thing should be undefined");
} else {
  console.log("    value?.thing is falsy: PASS");
}

if (!nullValue?.thing) {
  console.log("    !nullValue?.thing is falsy: PASS");
}

if (obj11?.nested?.deep) {
  console.log("    obj11?.nested?.deep exists: PASS");
}

if (!obj11?.missing?.deep) {
  console.log("    !obj11?.missing?.deep is falsy: PASS");
}

console.log("\n=== All tests completed ===");
