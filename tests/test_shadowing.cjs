// Test 1: Basic shadowing in nested scopes
let x = "outer";
{
    let x = "inner";
    Ant.println(x); // Should print "inner"
}
Ant.println(x); // Should print "outer"

// Test 2: Closure capturing outer scope
let y = 10;
function makeCounter() {
    let count = 0;
    return function() {
        count = count + 1;
        return count + y; // Access both local and outer scope
    };
}
let counter = makeCounter();
Ant.println(counter()); // Should print 11
Ant.println(counter()); // Should print 12

// Test 3: Variable shadowing in function
let z = "global";
function testShadow() {
    let z = "local";
    return z;
}
Ant.println(testShadow()); // Should print "local"
Ant.println(z); // Should print "global"

// Test 4: Nested function closures
function outer() {
    let a = 1;
    function middle() {
        let b = 2;
        function inner() {
            let c = 3;
            return a + b + c; // Access all three scopes
        }
        return inner();
    }
    return middle();
}
Ant.println(outer()); // Should print 6

// Test 5: Multiple closures sharing same outer scope
function makeCounters() {
    let shared = 0;
    return {
        inc: function() { shared = shared + 1; return shared; },
        dec: function() { shared = shared - 1; return shared; },
        get: function() { return shared; }
    };
}
let counters = makeCounters();
Ant.println(counters.inc()); // Should print 1
Ant.println(counters.inc()); // Should print 2
Ant.println(counters.dec()); // Should print 1
Ant.println(counters.get()); // Should print 1
