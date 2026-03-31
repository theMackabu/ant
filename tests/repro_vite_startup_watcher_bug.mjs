// Repro: Vite dev server startup triggers watcher-side
// `TypeError: undefined is not a function` inside chokidar/fsevents.
//
// Run with:
//   ant tests/repro_vite_startup_watcher_bug.mjs

import * as vite from '/Users/themackabu/.ant/pkg/exec/vite/node_modules/vite/dist/node/index.js';

process.on('unhandledRejection', (reason) => {
  console.log('[unhandledRejection]', reason?.name, reason?.message);
  if (reason?.stack) console.log(reason.stack);
});

const server = await vite.createServer({
  configFile: false,
  root: process.cwd(),
});

await server.listen();
console.log('listening');

setTimeout(async () => {
  console.log('done wait');
  await server.close();
}, 1500);
