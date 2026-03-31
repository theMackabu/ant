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
if (!config) process.exit(1);

const server = await run('createServer', () =>
  vite.createServer({ configFile: false, root: process.cwd() })
);
if (!server) process.exit(1);

// call each piece of initServer manually in the same order Vite does,
// so we can see which one hangs
console.log('\n-- manual initServer sequence --');

console.log('calling buildStart...');
await run('buildStart', () =>
  server.environments.client.pluginContainer.buildStart()
);

console.log('calling client.listen...');
await run('client.listen', () =>
  server.environments.client.listen(server)
);

console.log('calling ssr.listen...');
await run('ssr.listen', () =>
  server.environments.ssr.listen(server)
);

console.log('\n-- second pass (simulates initServer re-run) --');

console.log('calling buildStart (2nd)...');
await run('buildStart2', () =>
  server.environments.client.pluginContainer.buildStart()
);

console.log('calling client.listen (2nd)...');
await run('client.listen2', () =>
  server.environments.client.listen(server)
);

console.log('calling ssr.listen (2nd)...');
await run('ssr.listen2', () =>
  server.environments.ssr.listen(server)
);

console.log('\ndone');
