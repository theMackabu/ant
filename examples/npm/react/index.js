import { build } from 'esbuild';

const { outputFiles } = await build({
  entryPoints: ['app.jsx'],
  bundle: true,
  write: false,
  jsx: 'automatic',
  platform: 'node'
});

const code = outputFiles[0].text;
await import(`data:text/javascript,${encodeURIComponent(code)}`);
