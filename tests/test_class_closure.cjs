// Test class method closure
let x = 100;

class MyClass {
    constructor() {
        this.value = 10;
    }
    
    getX() {
        return x;
    }
    
    getSum() {
        return this.value + x;
    }
}

let obj = new MyClass();
console.log(obj.getX());   // Should print 100
console.log(obj.getSum()); // Should print 110
