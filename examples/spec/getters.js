import { test, summary } from './helpers.js';

console.log('Getter/Setter Tests\n');

let obj1 = {
  get value() {
    return 42;
  }
};
test('basic getter', obj1.value, 42);

let obj2 = {
  _count: 0,
  get count() {
    return this._count;
  },
  set count(val) {
    this._count = val;
  }
};
test('getter initial value', obj2.count, 0);
obj2.count = 10;
test('setter updates value', obj2.count, 10);

let obj3 = {
  firstName: 'John',
  lastName: 'Doe',
  get fullName() {
    return this.firstName + ' ' + this.lastName;
  }
};
test('computed getter', obj3.fullName, 'John Doe');

let obj4 = {
  get 'special-key'() {
    return 'special';
  }
};
test('string key getter', obj4['special-key'], 'special');

let obj5 = {
  get 0() {
    return 'zero';
  }
};
test('number key getter', obj5[0], 'zero');

let obj6 = {
  _x: 0,
  _y: 0,
  get x() {
    return this._x;
  },
  set x(v) {
    this._x = v;
  },
  get y() {
    return this._y;
  },
  set y(v) {
    this._y = v;
  }
};
obj6.x = 5;
obj6.y = 10;
test('multiple getters x', obj6.x, 5);
test('multiple getters y', obj6.y, 10);

let obj7 = {
  get data() {
    return { a: 1, b: 2 };
  }
};
test('getter returns object', obj7.data.a, 1);

let obj8 = {
  _age: 0,
  get age() {
    return this._age;
  },
  set age(val) {
    if (val < 0) val = 0;
    this._age = val;
  }
};
obj8.age = -5;
test('setter validation', obj8.age, 0);
obj8.age = 25;
test('setter valid value', obj8.age, 25);

let obj9 = {
  get readonly() {
    return 'constant';
  }
};
test('getter-only read', obj9.readonly, 'constant');
obj9.readonly = 'changed';
test('getter-only unchanged', obj9.readonly, 'constant');

let strictError = (function () {
  'use strict';
  try {
    let strictObj = {
      get x() {
        return 1;
      }
    };
    strictObj.x = 2;
    return false;
  } catch (e) {
    return e instanceof TypeError;
  }
})();
test('strict mode getter-only throws', strictError, true);

let obj10 = {
  multiplier: 3,
  _value: 7,
  get computed() {
    return this._value * this.multiplier;
  }
};
test('getter with this', obj10.computed, 21);

let sideEffect = 0;
let obj11 = {
  set trigger(val) {
    sideEffect = val * 2;
  }
};
obj11.trigger = 5;
test('setter side effect', sideEffect, 10);

summary();
