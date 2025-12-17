const user = {
  name: 'Theo',
  greet: function () {
    console.log('Hello, ' + this.name);
    console.log(this);

    console.log('\nexpecting error below');
    console.log(this.crypto.randomUUID());

    console.log(globalThis.Ant.version);
    console.log(window.crypto.randomUUID());
  }
};

user.greet();
