// Test closures in blocks
let result;
{
    let x = 5;
    {
        let y = 10;
        result = function() {
            return x + y;
        };
    }
}
console.log(result()); // Should print 15
