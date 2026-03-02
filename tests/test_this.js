const user = {
  name: 'Theo',
  greet: function () {
    console.log('Hello, ' + this.name);
    console.log(this);

    console.log(globalThis.Ant.version);
    try {
      console.log(this.crypto.randomUUID());
    } catch (err) {
      console.log(err);
    }
    console.log(window.crypto.randomUUID());
  }
};

user.greet();
