import { test, summary } from './helpers.js';

console.log('Proxy Tests\n');

const target = { a: 1, b: 2 };
const handler = {
  get(obj, prop) {
    return prop in obj ? obj[prop] : 'default';
  }
};
const proxy = new Proxy(target, handler);

test('proxy get existing', proxy.a, 1);
test('proxy get missing', proxy.c, 'default');

const setHandler = {
  set(obj, prop, value) {
    obj[prop] = value * 2;
    return true;
  }
};
const setProxy = new Proxy({}, setHandler);
setProxy.x = 5;
test('proxy set', setProxy.x, 10);

const hasHandler = {
  has(obj, prop) {
    return prop.startsWith('_') ? false : prop in obj;
  }
};
const hasProxy = new Proxy({ _private: 1, public: 2 }, hasHandler);
test('proxy has public', 'public' in hasProxy, true);
test('proxy has private', '_private' in hasProxy, false);

const deleteHandler = {
  deleteProperty(obj, prop) {
    if (prop.startsWith('_')) return false;
    delete obj[prop];
    return true;
  }
};
const delProxy = new Proxy({ _keep: 1, remove: 2 }, deleteHandler);
delete delProxy.remove;
delete delProxy._keep;
test('proxy delete allowed', delProxy.remove, undefined);
test('proxy delete blocked', delProxy._keep, 1);

const revocable = Proxy.revocable({ x: 1 }, {});
test('revocable proxy get', revocable.proxy.x, 1);
revocable.revoke();

summary();
