import fs from 'node:fs';

for (const name of ['access', 'readFile', 'readdir', 'realpath', 'rm', 'stat']) {
  const original = fs.promises?.[name];
  if (typeof original !== 'function') continue;
  fs.promises[name] = async function (...args) {
    try {
      return await original.apply(this, args);
    } catch (error) {
      console.log(`fs.promises.${name}`, args[0], error?.name, error?.message);
      throw error;
    }
  };
}

process.on('unhandledRejection', (reason) => {
  console.log('unhandledRejection', reason?.name, reason?.message);
  if (reason?.stack) console.log(reason.stack);
});

const vite = await import('/Users/themackabu/.ant/pkg/exec/vite/node_modules/vite/dist/node/index.js');
const server = await vite.createServer({ configFile: false, root: process.cwd() });

try {
  await server.listen();
  console.log('listen ok');
} catch (error) {
  console.log('listen', error?.name, error?.message);
  if (error?.stack) console.log(error.stack);
}
