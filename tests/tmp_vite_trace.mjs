const vite = await import('/Users/themackabu/.ant/pkg/exec/vite/node_modules/vite/dist/node/index.js');
const chunk = await import('/Users/themackabu/.ant/pkg/exec/vite/node_modules/vite/dist/node/chunks/node.js');
const config = await vite.resolveConfig({ configFile: false, root: process.cwd() }, 'serve');
const env = new vite.DevEnvironment('client', config, {
  hot: true,
  transport: vite.createServerHotChannel(),
  disableDepsOptimizer: true
});

function tryStringify(label, value) {
  try {
    const out = JSON.stringify(value, (_, item) => {
      if (typeof item === 'function' || item instanceof RegExp) return item.toString();
      return item;
    });
    console.log(label, 'ok', typeof out, out?.length ?? 0);
  } catch (error) {
    console.log(label, error?.name, error?.message);
  }
}

tryStringify('env.config.resolve', env.config.resolve);
tryStringify('env.config.assetsInclude', env.config.assetsInclude);
tryStringify('env.config.plugins', env.config.plugins.map((p) => p.name));
tryStringify('env.config.optimizeDeps.include', env.config.optimizeDeps.include);
tryStringify('env config-hash-shape', {
  define: !env.config.keepProcessEnv ? process.env.NODE_ENV || env.config.mode : null,
  root: env.config.root,
  resolve: env.config.resolve,
  assetsInclude: env.config.assetsInclude,
  plugins: env.config.plugins.map((p) => p.name),
  optimizeDeps: {
    include: env.config.optimizeDeps.include,
    exclude: env.config.optimizeDeps.exclude,
    rolldownOptions: {
      ...env.config.optimizeDeps.rolldownOptions,
      plugins: undefined,
      onLog: undefined,
      onwarn: undefined,
      checks: undefined,
      output: {
        ...env.config.optimizeDeps.rolldownOptions?.output,
        plugins: undefined
      }
    }
  },
  optimizeDepsPluginNames: env.config.optimizeDepsPluginNames
});

try {
  const metadata = chunk.at(env, String(Date.now()));
  console.log('initDepsOptimizerMetadata ok', typeof metadata);
} catch (error) {
  console.log('initDepsOptimizerMetadata', error?.name, error?.message);
}
