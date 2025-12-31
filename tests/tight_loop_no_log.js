function Foo() {
  return {
    lookup: function (key) {
      return key;
    }
  };
}

const foo = new Foo();
for (let i = 0; i < 40000; i = i + 1) {
  foo.lookup('a');
  foo.lookup('b');
  foo.lookup('c');
}
console.log('Done!');
