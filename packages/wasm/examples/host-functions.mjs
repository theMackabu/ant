import { Ant } from '../dist/index.js';

const ant = await Ant.create({
  globals: {
    greet: name => `Hello, ${name}!`,
    multiply: (left, right) => left * right,
    host: {
      application: 'checkout',
      region: 'local'
    }
  }
});

try {
  const result = await ant.eval(`
    ({
      greeting: greet("Silver"),
      total: multiply(6, 7),
      application: host.application,
    })
  `);

  console.log(result);
} finally {
  ant.dispose();
}
