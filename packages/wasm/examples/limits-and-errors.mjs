import { Ant, AntTimeoutError } from '../dist/index.js';

const ant = await Ant.create({
  memoryLimit: 32 * 1024 * 1024,
  timeout: 25
});

try {
  try {
    await ant.eval(`throw new TypeError("Invalid plugin configuration")`);
  } catch (error) {
    console.log('guest error:', error.name, error.message);
  }

  try {
    await ant.eval(`
      const value = {};
      value.self = value;
      value
    `);
  } catch (error) {
    console.log('transfer error:', error.message);
  }

  try {
    await ant.eval('while (true) {}');
  } catch (error) {
    if (!(error instanceof AntTimeoutError)) throw error;
    console.log('timeout:', error.message);
  }
} finally {
  ant.dispose();
}
