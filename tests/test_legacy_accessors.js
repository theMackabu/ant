let pass = true;

const target = {};
let seen = 0;

target.__defineSetter__('parseInt8', function (val) {
  seen = val ? 1 : -1;
});

target.parseInt8 = true;
if (seen !== 1) {
  console.log('FAIL: __defineSetter__ should install a working setter');
  pass = false;
}

const setter = target.__lookupSetter__('parseInt8');
if (typeof setter !== 'function') {
  console.log('FAIL: __lookupSetter__ should return the setter function');
  pass = false;
}

const proto = {};
proto.__defineGetter__('value', function () {
  return 42;
});

const child = Object.create(proto);
const getter = child.__lookupGetter__('value');
if (typeof getter !== 'function') {
  console.log('FAIL: __lookupGetter__ should find getters on the prototype chain');
  pass = false;
}

if (child.value !== 42) {
  console.log('FAIL: getter installed with __defineGetter__ should be used');
  pass = false;
}

if (pass) console.log('PASS');
