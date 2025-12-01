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
Ant.println('This should not print');
