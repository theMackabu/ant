const Bar = class bar {
  static name() {}
};

delete Bar.name;

console.log(Bar.name); // expected '' got undefined
console.log(typeof Bar.name); // expected 'string' got 'undefined'
