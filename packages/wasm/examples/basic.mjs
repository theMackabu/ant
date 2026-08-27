import { Ant } from '../dist/index.js';

const ant = await Ant.create();

try {
  const squares = await ant.eval(`
    Array.from({ length: 5 }, (_, index) => index ** 2)
  `);

  console.log(squares);
} finally {
  ant.dispose();
}
