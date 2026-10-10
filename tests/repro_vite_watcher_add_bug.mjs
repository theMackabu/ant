// Repro: Vite's chokidar watcher path triggers
// `TypeError: undefined is not a function` without needing full server startup.
//
// Run with:
//   ant tests/repro_vite_watcher_add_bug.mjs

import { existsSync as viteExists } from 'node:fs';
import { homedir as viteHome } from 'node:os';
const viteModules = `${viteHome()}/.ant/pkg/exec/vite/node_modules`;
if (!viteExists(viteModules)) {
  console.log(`skip: no Vite install at ${viteModules}`);
  process.exit(0);
}
const vite = await import(`${viteModules}/vite/dist/node/index.js`);

process.on('unhandledRejection', (reason) => {
  console.log('[unhandledRejection]', reason?.name, reason?.message);
  if (reason?.stack) console.log(reason.stack);
});

const server = await vite.createServer({
  configFile: false,
  root: process.cwd(),
  optimizeDeps: { noDiscovery: true, entries: [] },
});

server.watcher.add([
  '.env.local',
  '.env.development',
  '.env.development.local',
  '.definitely-missing',
]);

console.log('watcher.add issued');

setTimeout(async () => {
  console.log('done wait');
  await server.close();
}, 1500);
