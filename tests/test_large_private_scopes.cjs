const assert = require('node:assert');

const builders = {
  privates(n) { let s = 'class C {'; for (let i = 0; i < n; i++) s += `#p${i} = ${i};`; s += `m() { return this.#p0 + this.#p${n - 1} + this.#p${n >> 1} } static has(o) { return #p${n - 1} in o } } return [new C().m(), C.has(new C())]`; return new Function(s)(); },
  privateMethods(n) { let s = 'class C {'; for (let i = 0; i < n; i++) s += `#m${i}() { return ${i} }`; s += `m() { return this.#m0() + this.#m${n - 1}() } } return new C().m()`; return new Function(s)(); },
};

const expected = {"65535":{"privates":[98301,true],"privateMethods":65534},"65536":{"privates":[98303,true],"privateMethods":65535},"70000":{"privates":[104999,true],"privateMethods":69999}};

for (const [count, results] of Object.entries(expected)) {
  for (const [name, want] of Object.entries(results)) {
    assert.deepStrictEqual(builders[name](+count), want, `${name} with ${count} bindings`);
  }
}

console.log('ok');
