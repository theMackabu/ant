import { build } from 'esbuild';
import { join } from 'node:path';

const root = import.meta.dirname;

await build({
  entryPoints: [join(root, './src/index.js')],
  bundle: true,
  platform: 'browser',
  format: 'esm',
  define: {
    'process.env.NODE_ENV': JSON.stringify('production')
  },
  outfile: join(root, 'src/bin/worker.js')
});
