const vite = await import('/Users/themackabu/.ant/pkg/exec/vite/node_modules/vite/dist/node/index.js');

async function run(label, fn) {
  try {
    const value = await fn();
    console.log(label, 'ok', typeof value);
    return value;
  } catch (error) {
    console.log(label, error?.name, error?.message);
    if (error?.stack) console.log(error.stack);
    return null;
  }
}

const config = await run('resolveConfig', () =>
  vite.resolveConfig({ configFile: false, root: process.cwd() }, 'serve')
);

if (config) {
  const server = await run('createServer', () =>
    vite.createServer({ configFile: false, root: process.cwd() })
  );

  if (server) {
    const httpServer = server.httpServer;

    // Patch origListen (Vite's async wrapper) to catch the inner rejection
    const viteAsyncWrapper = httpServer.listen;
    httpServer.listen = function(...args) {
      console.log('[trace] httpServer.listen called, port=', args[0], 'host=', args[1]);
      const p = viteAsyncWrapper.apply(this, args);
      if (p && typeof p.then === 'function') {
        p.then(
          () => console.log('[trace] async wrapper resolved'),
          (e) => console.log('[trace] async wrapper rejected:', e?.message)
        );
      }
      return p;
    };

    await run('server.listen', async () => {
      await server.listen();
      await server.close();
      return server;
    });
  }
}
