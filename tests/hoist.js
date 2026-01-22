function hoist1() {
  b(); // b is hoisted from below
  function b() {
    console.log('b');
  }
}

function hoist2() {
  b(); // b exists but is undefined
  var b = function b() {
    console.log('b');
  };
}

function hoist3() {
  b(); // b does not exist
  let b = function b() {
    console.log('b');
  };
}

try {
  hoist1();
} catch (err) {
  console.log(err);
}

try {
  hoist2();
} catch (err) {
  console.log(err);
}

try {
  hoist3();
} catch (err) {
  console.log(err);
}
