function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function makeObject(which, value) {
  const dynamic = `dynamic${which}`;
  switch (which) {
    case 0: return { alpha0: value, omega0: value + 1 };
    case 1: return { [dynamic]: value, tail1: value + 1 };
    case 2: return { alpha2: value, middle2: value + 1, omega2: value + 2 };
    case 3: return { [dynamic]: value, tail3: value + 1 };
    case 4: return { alpha4: value };
    case 5: return { [dynamic]: value, tail5: value + 1 };
    case 6: return { alpha6: value, omega6: value + 1 };
    default: return { [dynamic]: value, tail7: value + 1 };
  }
}

function makeObjectInterpreted(which, value) {
  debugger;
  const dynamic = `dynamic${which}`;
  switch (which) {
    case 0: return { alpha0: value, omega0: value + 1 };
    case 1: return { [dynamic]: value, tail1: value + 1 };
    case 2: return { alpha2: value, middle2: value + 1, omega2: value + 2 };
    case 3: return { [dynamic]: value, tail3: value + 1 };
    case 4: return { alpha4: value };
    case 5: return { [dynamic]: value, tail5: value + 1 };
    case 6: return { alpha6: value, omega6: value + 1 };
    default: return { [dynamic]: value, tail7: value + 1 };
  }
}

function checkObject(which, value, object, mode) {
  const dynamic = `dynamic${which}`;
  let keys;
  let lastValue;
  switch (which) {
    case 0: keys = 'alpha0,omega0'; lastValue = object.omega0; break;
    case 1: keys = `${dynamic},tail1`; lastValue = object.tail1; break;
    case 2: keys = 'alpha2,middle2,omega2'; lastValue = object.omega2; break;
    case 3: keys = `${dynamic},tail3`; lastValue = object.tail3; break;
    case 4: keys = 'alpha4'; lastValue = object.alpha4; break;
    case 5: keys = `${dynamic},tail5`; lastValue = object.tail5; break;
    case 6: keys = 'alpha6,omega6'; lastValue = object.omega6; break;
    default: keys = `${dynamic},tail7`; lastValue = object.tail7; break;
  }
  assert(Object.keys(object).join(',') === keys, `${mode} site ${which} keys`);
  assert(object[Object.keys(object)[0]] === value, `${mode} site ${which} first value`);
  const expectedLast = which === 2 ? value + 2 : which === 4 ? value : value + 1;
  assert(lastValue === expectedLast, `${mode} site ${which} last value`);
}

let checksum = 0;
for (let i = 0; i < 20000; i++) {
  const which = i & 7;
  const object = makeObject(which, i);
  checksum += object[Object.keys(object)[0]];
}
assert(checksum === 199990000, 'warmed object-site checksum');

for (let which = 0; which < 8; which++) {
  checkObject(which, 100 + which, makeObject(which, 100 + which), 'jit');
  checkObject(
    which,
    200 + which,
    makeObjectInterpreted(which, 200 + which),
    'interpreter'
  );
}

console.log('object site lookup ok');
