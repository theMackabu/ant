import { nanoid } from 'nanoid';
import { join } from 'node:path';
import * as esbuild from 'esbuild';

const args = [`id=${nanoid()}`];
const replacements: Record<string, string> = {};

for (const arg of args) {
  const pivot = arg.indexOf('=');
  if (pivot === -1) continue;

  const key = arg.slice(0, pivot);
  const value = arg.slice(pivot + 1);

  replacements[`import.meta.env.${key}`] = JSON.stringify(value);
}

const result = await esbuild.build({
  entryPoints: [join(import.meta.dirname, 'app.jsx')],
  bundle: true,
  write: false,
  define: replacements
});

if (result.outputFiles?.length) {
  console.log(result.outputFiles[0].text);
}
