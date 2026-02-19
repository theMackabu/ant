// for loop
function testFor() {
    let x = 0
    for (let i = 0; i < 3; i++) x += 1
    return x
}

// while loop
function testWhile() {
    let x = 0
    let i = 0
    while (i < 3) i++, x += 1
    return x
}

// if statement
function testIf() {
    let x = 10
    if (x > 5) x = 42
    return x
}

// if-else
function testIfElse() {
    let x = 3
    if (x > 5) x = 100
    else x = 200
    return x
}

// nested
function testNested() {
    let sum = 0
    for (let i = 0; i < 2; i++) for (let j = 0; j < 2; j++) sum += 1
    return sum
}

console.log("for:", testFor())         // 3
console.log("while:", testWhile())     // 3
console.log("if:", testIf())           // 42
console.log("if-else:", testIfElse())  // 200
console.log("nested:", testNested())   // 4
