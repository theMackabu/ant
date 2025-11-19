// Test function declaration closure
function outer() {
    let x = 1;
    
    function inner() {
        return x;
    }
    return inner;
}

let fn = outer();
Ant.println("Result: " + fn());
