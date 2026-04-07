const msg = Ant.match(process.argv[2], {
  200: 'ok',
  404: 'not found',
  [$ >= 500 && $ < 600]: 'server error',
  [Symbol.default]: code => {
    console.log('unexpected code');
    return `unknown value ${code}`;
  }
});

console.log(msg);
