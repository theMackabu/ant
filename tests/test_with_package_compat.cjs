const assert = (condition, message) => {
  if (!condition) throw new Error(message);
};

const assigned = () => 1;
const array = [() => 2];
function sourceOf(callback) {
  return callback.toString();
}

assert(sourceOf(assigned) === '() => 1', 'assigned expression arrow source should exclude semicolon');
assert(sourceOf(array[0]) === '() => 2', 'array expression arrow source should exclude bracket');
assert(sourceOf(() => 3) === '() => 3', 'argument expression arrow source should exclude parenthesis');

const target = [1, 2, 3];
assert(Reflect.has(target, 'toString'), 'Reflect.has should find inherited array methods');
const proxy = new Proxy(target, {
  has(target, property) {
    return Reflect.has(target, property);
  },
  get(target, property, receiver) {
    return Reflect.get(target, property, receiver);
  },
});

assert(proxy.toString() === '1,2,3', 'Reflect.get should find inherited array methods for a proxy receiver');
const withResult = new Function('with(arguments[0]){return toString()}')(proxy);
assert(withResult === '1,2,3', 'with-bound identifier calls should preserve the proxy receiver');

console.log('with package compatibility tests passed');
