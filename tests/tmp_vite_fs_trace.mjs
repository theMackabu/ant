import { existsSync as viteExists } from 'node:fs';
import { homedir as viteHome } from 'node:os';
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

const viteModules = `${viteHome()}/.ant/pkg/exec/vite/node_modules`;
if (!viteExists(viteModules)) {
  console.log(`skip: no Vite install at ${viteModules}`);
  process.exit(0);
}
const vite = await import(`${viteModules}/vite/dist/node/index.js`);
const server = await vite.createServer({ configFile: false, root: process.cwd() });

try {
  await server.listen();
  console.log('listen ok');
} catch (error) {
  console.log('listen', error?.name, error?.message);
  if (error?.stack) console.log(error.stack);
}
