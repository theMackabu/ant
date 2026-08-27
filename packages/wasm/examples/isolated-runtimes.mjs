import { Ant } from '../dist/index.js';

const [left, right] = await Promise.all([
  Ant.create({ globals: { label: 'left' }}),
  Ant.create({ globals: { label: 'right' }})
]);

try {
  await left.eval('globalThis.count = 10');
  await right.eval('globalThis.count = 100');

  await left.eval('count += 1');
  await right.eval('count += 5');

  console.log({
    left: await left.eval('({ label, count })'),
    right: await right.eval('({ label, count })')
  });
} finally {
  left.dispose();
  right.dispose();
}
