// Repro: Vite's chokidar watcher path triggers
// `TypeError: undefined is not a function` without needing full server startup.
//
// Run with:
//   ant tests/repro_vite_watcher_add_bug.mjs

import * as vite from '/Users/themackabu/.ant/pkg/exec/vite/node_modules/vite/dist/node/index.js';

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
