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

let boxedHits = 0;
String.prototype.__defineGetter__('boxedLegacyAccessorProbe', function () {
  boxedHits++;
  return 'ok';
});

try {
  const boxedGetter = Object.prototype.__lookupGetter__.call('abc', 'boxedLegacyAccessorProbe');
  if (typeof boxedGetter !== 'function') {
    console.log('FAIL: primitive receivers should be boxed for __lookupGetter__');
    pass = false;
  }

  const defineResult = Object.prototype.__defineGetter__.call('abc', 'ephemeral', function () {
    return 1;
  });
  if (defineResult !== undefined) {
    console.log('FAIL: __defineGetter__ on boxed primitive should return undefined');
    pass = false;
  }

  if ('abc'.boxedLegacyAccessorProbe !== 'ok' || boxedHits !== 1) {
    console.log('FAIL: boxed primitive getter should resolve through String.prototype');
    pass = false;
  }
} finally {
  delete String.prototype.boxedLegacyAccessorProbe;
}

let proxyDefined = false;
let proxyLookedUp = false;
const proxyTarget = {};
const proxy = new Proxy(proxyTarget, {
  defineProperty(target, key, desc) {
    proxyDefined = key === 'legacyProxy' && typeof desc.get === 'function';
    Object.defineProperty(target, key, desc);
    return true;
  },
  getOwnPropertyDescriptor(target, key) {
    if (key === 'legacyProxy') proxyLookedUp = true;
    return Object.getOwnPropertyDescriptor(target, key);
  }
});

proxy.__defineGetter__('legacyProxy', function () {
  return 7;
});

const proxyGetter = proxy.__lookupGetter__('legacyProxy');
if (!proxyDefined) {
  console.log('FAIL: proxy __defineGetter__ should go through defineProperty semantics');
  pass = false;
}

if (!proxyLookedUp) {
  console.log('FAIL: proxy __lookupGetter__ should consult getOwnPropertyDescriptor');
  pass = false;
}

if (typeof proxyGetter !== 'function') {
  console.log('FAIL: proxy __lookupGetter__ should return the getter function');
  pass = false;
}

if (proxy.legacyProxy !== 7) {
  console.log('FAIL: proxy getter installed with __defineGetter__ should be used');
  pass = false;
}

if (pass) console.log('PASS');
