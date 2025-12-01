class Meow extends Error {
  constructor() {
    super('meow');
    this.name = 'MeowError';
  }
}

function meow() {
  throw new Meow();
}

meow();
console.log('This should not print');
