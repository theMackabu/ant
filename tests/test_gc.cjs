// Test Ant.gc(), Ant.alloc(), and Ant.stats()

console.log("=== Testing Ant.alloc() ===");
let alloc1 = Ant.alloc();
console.log("Initial allocation:");
console.log("  used:", alloc1.used);
console.log("  minFree:", alloc1.minFree);

console.log("\n=== Creating objects to allocate memory ===");
let arr = [];
for (let i = 0; i < 100; i = i + 1) {
    arr.push({ value: i, name: "item" + i });
}
console.log("Created array with 100 objects");

let alloc2 = Ant.alloc();
console.log("After allocation:");
console.log("  used:", alloc2.used);
console.log("  minFree:", alloc2.minFree);
console.log("  increase:", alloc2.used - alloc1.used);

console.log("\n=== Testing Ant.stats() ===");
let stats1 = Ant.stats();
console.log("Memory stats:");
console.log("  used:", stats1.used);
console.log("  minFree:", stats1.minFree);
console.log("  cstack:", stats1.cstack);

console.log("\n=== Testing Ant.gc() ===");
// Clear reference to allow GC
arr = null;

let gcResult = Ant.gc();
console.log("GC result:");
console.log("  before:", gcResult.before);
console.log("  after:", gcResult.after);
console.log("  freed:", gcResult.before - gcResult.after);

console.log("\n=== Verifying memory after GC ===");
let alloc3 = Ant.alloc();
console.log("After GC:");
console.log("  used:", alloc3.used);
console.log("  minFree:", alloc3.minFree);

console.log("\n=== Testing multiple GC cycles ===");
// Allocate and free multiple times
for (let cycle = 0; cycle < 3; cycle = cycle + 1) {
    console.log("Cycle", cycle + 1);
    
    // Allocate
    let temp = [];
    for (let i = 0; i < 50; i = i + 1) {
        temp.push({ data: "test data " + i });
    }
    let beforeGc = Ant.alloc();
    console.log("  Before GC - used:", beforeGc.used);
    
    // Clear and collect
    temp = null;
    let gc = Ant.gc();
    console.log("  After GC - used:", gc.after, "freed:", gc.before - gc.after);
}

console.log("\n=== Testing stats consistency ===");
let statsA = Ant.stats();
let allocA = Ant.alloc();
console.log("Stats and alloc should match:");
console.log("  stats.used:", statsA.used);
console.log("  alloc.used:", allocA.used);
console.log("  match:", statsA.used === allocA.used);

console.log("\n=== Test complete ===");
