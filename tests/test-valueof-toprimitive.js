// ============================================
// TEST: valueOf and Symbol.toPrimitive behavior
// ============================================
// Bug: valueOf is never called, toPrimitive may not work
// ============================================

console.log('=== valueOf AND Symbol.toPrimitive TESTS ===\n');

// --- valueOf TESTS ---

console.log('SECTION 1: valueOf\n');

console.log('1. Only valueOf, returns string:');
const obj1 = {
  valueOf: function () {
    return 'from-valueOf';
  }
};
console.log("   expected concat: 'from-valueOf'");
console.log("   concat:          '" + ('' + obj1) + "'");
console.log("   String():        '" + String(obj1) + "'");

console.log('\n2. Only valueOf, returns number:');
const obj2 = {
  valueOf: function () {
    return 42;
  }
};
console.log("   expected concat: '42'");
console.log("   concat:          '" + ('' + obj2) + "'");
console.log("   String():        '" + String(obj2) + "'");

console.log('\n3. Only valueOf, returns boolean:');
const obj3 = {
  valueOf: function () {
    return true;
  }
};
console.log("   expected concat: 'true'");
console.log("   concat:          '" + ('' + obj3) + "'");

console.log('\n4. valueOf returns object (should fallback to toString):');
const obj4 = {
  valueOf: function () {
    return {};
  }
};
console.log("   expected: '[object Object]' (fallback)");
console.log("   concat:   '" + ('' + obj4) + "'");

console.log('\n5. valueOf returns null:');
const obj5 = {
  valueOf: function () {
    return null;
  }
};
console.log("   expected: 'null'");
console.log("   concat:   '" + ('' + obj5) + "'");

console.log('\n6. valueOf returns undefined:');
const obj6 = {
  valueOf: function () {
    return undefined;
  }
};
console.log("   expected: 'undefined'");
console.log("   concat:   '" + ('' + obj6) + "'");

// --- toString + valueOf priority ---

console.log('\n\nSECTION 2: toString vs valueOf priority\n');

console.log('7. Both toString and valueOf (user functions):');
const obj7 = {
  toString: function () {
    return 'from-toString';
  },
  valueOf: function () {
    return 'from-valueOf';
  }
};
console.log("   expected: 'from-toString' (toString has priority for string hint)");
console.log("   direct toString: '" + obj7.toString() + "'");
console.log("   direct valueOf:  '" + obj7.valueOf() + "'");
const result7 = '' + obj7;
console.log("   concat:          '" + result7 + "'");
console.log('   (if missing, CRASH)');

console.log('\n8. toString throws, valueOf works:');
const obj8 = {
  toString: function () {
    throw new Error('toString error');
  },
  valueOf: function () {
    return 'from-valueOf';
  }
};
console.log("   expected: 'from-valueOf' (fallback) or Error");
try {
  console.log("   concat:   '" + ('' + obj8) + "'");
} catch (e) {
  console.log('   error:    ' + e.message);
}

console.log('\n9. toString returns object, valueOf returns string:');
const obj9 = {
  toString: function () {
    return {};
  },
  valueOf: function () {
    return 'from-valueOf';
  }
};
console.log("   expected: 'from-valueOf' (fallback)");
try {
  console.log("   concat:   '" + ('' + obj9) + "'");
} catch (e) {
  console.log('   error:    ' + e.message);
}

console.log('\n10. Both return objects (should throw TypeError):');
const obj10 = {
  toString: function () {
    return {};
  },
  valueOf: function () {
    return {};
  }
};
console.log('   expected: TypeError');
try {
  console.log("   concat:   '" + ('' + obj10) + "'");
} catch (e) {
  console.log('   error:    ' + e.name + ': ' + e.message);
}

// --- Symbol.toPrimitive ---

console.log('\n\nSECTION 3: Symbol.toPrimitive\n');

console.log('11. toPrimitive returns string:');
const obj11 = {};
obj11[Symbol.toPrimitive] = function (hint) {
  return 'primitive-' + hint;
};
console.log("   expected concat: 'primitive-default' or 'primitive-string'");
console.log("   concat:          '" + ('' + obj11) + "'");
console.log("   String():        '" + String(obj11) + "'");
console.log("   template:        '" + `${obj11}` + "'");

console.log('\n12. toPrimitive returns number:');
const obj12 = {};
obj12[Symbol.toPrimitive] = function (hint) {
  return 99;
};
console.log("   expected: '99'");
console.log("   concat:   '" + ('' + obj12) + "'");

console.log('\n13. toPrimitive overrides toString and valueOf:');
const obj13 = {
  toString: function () {
    return 'from-toString';
  },
  valueOf: function () {
    return 'from-valueOf';
  }
};
obj13[Symbol.toPrimitive] = function (hint) {
  return 'from-toPrimitive';
};
console.log("   expected: 'from-toPrimitive' (toPrimitive has highest priority)");
console.log("   concat:   '" + ('' + obj13) + "'");

console.log('\n14. toPrimitive returns object (should throw):');
const obj14 = {};
obj14[Symbol.toPrimitive] = function (hint) {
  return {};
};
console.log('   expected: TypeError');
try {
  console.log("   concat:   '" + ('' + obj14) + "'");
} catch (e) {
  console.log('   error:    ' + e.name + ': ' + e.message);
}

console.log('\n15. toPrimitive throws:');
const obj15 = {};
obj15[Symbol.toPrimitive] = function (hint) {
  throw new Error('toPrimitive error');
};
console.log('   expected: Error: toPrimitive error');
try {
  console.log("   concat:   '" + ('' + obj15) + "'");
} catch (e) {
  if (e && e.message) {
    console.log('   error:    ' + e.message);
  } else {
    console.log('   error:    ' + String(e));
  }
}

console.log('\n16. toPrimitive is not a function:');
const obj16 = {};
obj16[Symbol.toPrimitive] = 'not a function';
console.log('   expected: TypeError or fallback to toString');
try {
  console.log("   concat:   '" + ('' + obj16) + "'");
} catch (e) {
  console.log('   error:    ' + e.name + ': ' + e.message);
}

// --- Hint values ---

console.log('\n\nSECTION 4: Hint values\n');

console.log('17. Check hint values:');
const hints = [];
const obj17 = {};
obj17[Symbol.toPrimitive] = function (hint) {
  hints.push(hint);
  return 'ok';
};

console.log("   concat '' + obj:");
let r1 = '' + obj17;
console.log("     hint was: '" + hints[hints.length - 1] + "'");

console.log('   String(obj):');
let r2 = String(obj17);
console.log("     hint was: '" + hints[hints.length - 1] + "'");

console.log('   template `${obj}`:');
let r3 = `${obj17}`;
console.log("     hint was: '" + hints[hints.length - 1] + "'");

console.log('   +obj (unary plus, number hint):');
let r4 = +obj17;
console.log("     hint was: '" + hints[hints.length - 1] + "'");

console.log('\n   All hints collected: [' + hints.join(', ') + ']');
console.log("   Expected: ['default', 'string', 'string', 'number']");

console.log('\n=== DONE ===');
