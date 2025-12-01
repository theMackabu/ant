// Test object literal with method closure
let x = 100;

let obj = {
    method: function() {
        return x;
    }
};

console.log(obj.method()); // Should print 100
