String.prototype.meow = function () {
  if (!this) return 'meow';
  return `${this}, meow`;
};

console.log('hello'.meow());
