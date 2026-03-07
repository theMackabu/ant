import { build } from 'esbuild';
import { join } from 'node:path';

const { outputFiles } = await build({
  entryPoints: [join(import.meta.dirname, 'app.jsx')],
  bundle: true,
  write: false,
  jsx: 'automatic',
  platform: 'node'
});

const code = outputFiles[0].text;
await import(`data:text/javascript,${encodeURIComponent(code)}`);
