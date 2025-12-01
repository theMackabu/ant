// Test objects in if statements
// Tests property access and truthiness evaluation

Ant.println("=== Object in If Statements Test ===\n");

// Test 1: Basic object property truthiness
Ant.println("Test 1: Object property truthiness");
let thing = {
  hasThing: true,
  hasOther: false,
  count: 5,
  name: "test"
};

if (thing.hasThing) {
  Ant.println("  thing.hasThing is truthy: PASS");
}

if (!thing.hasOther) {
  Ant.println("  !thing.hasOther is falsy: PASS");
}

if (thing.count) {
  Ant.println("  thing.count (5) is truthy: PASS");
}

if (thing.name) {
  Ant.println("  thing.name ('test') is truthy: PASS");
}

// Test 2: Undefined and null properties
Ant.println("\nTest 2: Undefined and null properties");
let obj = {
  defined: "value",
  nullValue: null,
  undefinedValue: undefined,
  zeroValue: 0,
  emptyString: ""
};

if (!obj.nonExistent) {
  Ant.println("  !obj.nonExistent (undefined): PASS");
}

if (!obj.nullValue) {
  Ant.println("  !obj.nullValue (null): PASS");
}

if (!obj.undefinedValue) {
  Ant.println("  !obj.undefinedValue (undefined): PASS");
}

if (!obj.zeroValue) {
  Ant.println("  !obj.zeroValue (0): PASS");
}

if (!obj.emptyString) {
  Ant.println("  !obj.emptyString (''): PASS");
}

if (obj.defined) {
  Ant.println("  obj.defined ('value'): PASS");
}

// Test 3: Nested object properties
Ant.println("\nTest 3: Nested object properties");
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
  Ant.println("  config.settings exists: PASS");
}

if (config.settings.enabled) {
  Ant.println("  config.settings.enabled is true: PASS");
}

if (!config.settings.disabled) {
  Ant.println("  !config.settings.disabled is false: PASS");
}

if (config.settings.nested) {
  Ant.println("  config.settings.nested exists: PASS");
}

if (config.settings.nested.deep) {
  Ant.println("  config.settings.nested.deep has value: PASS");
}

// Test 4: Function properties
Ant.println("\nTest 4: Function properties");
let api = {
  hasMethod: function() {
    return "called";
  },
  noMethod: null
};

if (api.hasMethod) {
  Ant.println("  api.hasMethod exists:", api.hasMethod());
}

if (!api.noMethod) {
  Ant.println("  !api.noMethod is null: PASS");
}

// Test 5: Array properties
Ant.println("\nTest 5: Array properties");
let data = {
  items: [1, 2, 3],
  emptyItems: [],
  noItems: null
};

if (data.items) {
  Ant.println("  data.items exists, length:", data.items.length);
}

if (data.emptyItems) {
  Ant.println("  data.emptyItems exists but empty, length:", data.emptyItems.length);
}

if (!data.noItems) {
  Ant.println("  !data.noItems is null: PASS");
}

// Test 6: Conditional checks with property access
Ant.println("\nTest 6: Conditional property checks");
let user = {
  name: "John",
  age: 30,
  active: true
};

if (user.name && user.age) {
  Ant.println("  user.name && user.age both exist: PASS");
}

if (user.active && user.name) {
  Ant.println("  user.active && user.name both truthy: PASS");
}

if (!user.deleted || user.active) {
  Ant.println("  !user.deleted || user.active: PASS");
}

// Test 7: Return based on object property
Ant.println("\nTest 7: Return based on property");
function checkUser(user) {
  if (!user) return { error: "no user" };
  if (!user.name) return { error: "no name" };
  return { success: true, name: user.name };
}

Ant.println("  With null:", checkUser(null));
Ant.println("  With no name:", checkUser({ age: 25 }));
Ant.println("  With name:", checkUser({ name: "Alice" }));

// Test 8: Object itself as condition
Ant.println("\nTest 8: Object as condition");
let obj1 = { value: 1 };
let obj2 = null;
let obj3 = undefined;

if (obj1) {
  Ant.println("  obj1 (object) is truthy: PASS");
}

if (!obj2) {
  Ant.println("  !obj2 (null) is falsy: PASS");
}

if (!obj3) {
  Ant.println("  !obj3 (undefined) is falsy: PASS");
}

// Test 9: Boolean properties in complex conditions
Ant.println("\nTest 9: Complex boolean property checks");
let feature = {
  enabled: true,
  experimental: false,
  beta: true,
  stable: false
};

if (feature.enabled && feature.beta) {
  Ant.println("  enabled && beta: PASS");
}

if (feature.enabled && !feature.stable) {
  Ant.println("  enabled && !stable: PASS");
}

if (!feature.experimental && !feature.stable) {
  Ant.println("  !experimental && !stable: PASS");
}

// Test 10: Property chain with guard
Ant.println("\nTest 10: Safe property access");
function getValue(obj) {
  if (!obj) return "no object";
  if (!obj.data) return "no data";
  if (!obj.data.value) return "no value";
  return obj.data.value;
}

Ant.println("  With null:", getValue(null));
Ant.println("  With no data:", getValue({}));
Ant.println("  With no value:", getValue({ data: {} }));
Ant.println("  With value:", getValue({ data: { value: "found" } }));

// Test 11: Optional chaining with undefined/null values
Ant.println("\nTest 11: Optional chaining (?.)");
const value = undefined;
const nullValue = null;
const obj11 = { nested: { deep: "value" } };

Ant.println("  value?.thing (undefined):", value?.thing);
Ant.println("  nullValue?.thing (null):", nullValue?.thing);
Ant.println("  obj11?.nested?.deep:", obj11?.nested?.deep);
Ant.println("  obj11?.missing?.deep:", obj11?.missing?.deep);

if (value?.thing) {
  Ant.println("    FAIL: value?.thing should be undefined");
} else {
  Ant.println("    value?.thing is falsy: PASS");
}

if (!nullValue?.thing) {
  Ant.println("    !nullValue?.thing is falsy: PASS");
}

if (obj11?.nested?.deep) {
  Ant.println("    obj11?.nested?.deep exists: PASS");
}

if (!obj11?.missing?.deep) {
  Ant.println("    !obj11?.missing?.deep is falsy: PASS");
}

Ant.println("\n=== All tests completed ===");
