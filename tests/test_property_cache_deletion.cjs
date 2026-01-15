// Test: Property cache invalidation on deletion

console.log("=== Test 1: Mid-chain deletion ===");
{
  let obj = { a: 1, b: 2, c: 3 };
  console.log("obj.b before:", obj.b);  // populate cache
  delete obj.b;
  console.log("obj.b after delete:", obj.b);  // should be undefined
  console.log("PASS:", obj.b === undefined);
}

console.log("\n=== Test 2: First property deletion ===");
{
  let obj = {};
  obj.a = 1;
  obj.b = 2;
  obj.c = 3;  // c is first in chain (newest)
  console.log("obj.c before:", obj.c);
  delete obj.c;
  console.log("obj.c after delete:", obj.c);
  console.log("PASS:", obj.c === undefined);
}

console.log("\n=== Test 3: Last property deletion ===");
{
  let obj = {};
  obj.a = 1;  // a is last in chain (oldest)
  obj.b = 2;
  obj.c = 3;
  console.log("obj.a before:", obj.a);
  delete obj.a;
  console.log("obj.a after delete:", obj.a);
  console.log("PASS:", obj.a === undefined);
}

console.log("\n=== Test 4: Multiple deletions ===");
{
  let obj = { a: 1, b: 2, c: 3, d: 4, e: 5 };
  obj.b; obj.c; obj.d;  // populate cache
  delete obj.c;
  delete obj.b;
  delete obj.d;
  console.log("PASS:", obj.b === undefined && obj.c === undefined && obj.d === undefined);
  console.log("a still exists:", obj.a === 1);
  console.log("e still exists:", obj.e === 5);
}

console.log("\n=== Test 5: Delete and re-add ===");
{
  let obj = { a: 1, b: 2, c: 3 };
  obj.b;  // cache
  delete obj.b;
  console.log("after delete:", obj.b);
  obj.b = 999;
  console.log("after re-add:", obj.b);
  console.log("PASS:", obj.b === 999);
}

console.log("\n=== Test 6: In-place update doesnt break cache ===");
{
  let obj = { a: 1, b: 2, c: 3 };
  obj.b;  // cache
  obj.b = 100;  // in-place update
  obj.b = 200;  // another update
  console.log("obj.b:", obj.b);
  console.log("PASS:", obj.b === 200);
}

console.log("\n=== Test 7: Many properties stress test ===");
{
  let obj = {};
  for (let i = 0; i < 100; i++) {
    obj["prop" + i] = i;
  }
  // Access some to populate cache
  obj.prop50; obj.prop25; obj.prop75;
  
  // Delete from various positions
  delete obj.prop50;
  delete obj.prop25;
  delete obj.prop75;
  delete obj.prop0;   // last in chain
  delete obj.prop99;  // first in chain
  
  console.log("PASS:", 
    obj.prop50 === undefined && 
    obj.prop25 === undefined && 
    obj.prop75 === undefined &&
    obj.prop0 === undefined &&
    obj.prop99 === undefined &&
    obj.prop1 === 1 &&
    obj.prop98 === 98
  );
}

console.log("\n=== Test 8: Delete non-existent (should not crash) ===");
{
  let obj = { a: 1 };
  obj.a;  // cache
  delete obj.nonexistent;  // should be no-op
  console.log("PASS:", obj.a === 1);
}

console.log("\n=== Test 9: Repeated access after delete ===");
{
  let obj = { a: 1, b: 2, c: 3 };
  obj.b; obj.b; obj.b;  // multiple cache hits
  delete obj.b;
  let results = [obj.b, obj.b, obj.b];  // multiple accesses after delete
  console.log("PASS:", results.every(x => x === undefined));
}

console.log("\n=== Test 10: Array element deletion ===");
{
  let arr = [1, 2, 3, 4, 5];
  arr[2];  // cache
  delete arr[2];
  console.log("arr[2]:", arr[2]);
  console.log("PASS:", arr[2] === undefined && arr.length === 5);
}

console.log("\n=== All tests complete ===");
