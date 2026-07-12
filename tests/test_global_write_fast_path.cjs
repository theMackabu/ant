const assert = (condition, message) => {
  if (!condition) throw new Error(message);
};

globalThis.fastGlobalData = 1;
fastGlobalData = 2;
assert(globalThis.fastGlobalData === 2,
  'global data-property assignment should update the own property');

let setterReceiver;
let setterValue;
Object.defineProperty(globalThis, 'fastGlobalSetter', {
  configurable: true,
  set(value) {
    setterReceiver = this;
    setterValue = value;
  },
});
fastGlobalSetter = 3;
assert(setterReceiver === globalThis && setterValue === 3,
  'global accessor assignment should use the realm global as receiver');

Object.defineProperty(globalThis, 'fastGlobalReadonly', {
  configurable: true,
  writable: false,
  value: 4,
});
fastGlobalReadonly = 5;
assert(fastGlobalReadonly === 4,
  'sloppy assignment should not change a readonly global property');

let strictReadonlyThrew = false;
try {
  (function () {
    'use strict';
    fastGlobalReadonly = 5;
  })();
} catch (error) {
  strictReadonlyThrew = error instanceof TypeError;
}
assert(strictReadonlyThrew,
  'strict assignment should reject a readonly global property');

delete globalThis.fastGlobalData;
delete globalThis.fastGlobalSetter;
delete globalThis.fastGlobalReadonly;

console.log('global write fast-path tests passed');
