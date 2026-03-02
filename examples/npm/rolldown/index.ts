import { nanoid } from 'nanoid';
import { join } from 'node:path';
import { rolldown } from 'rolldown';

const args = [`id=${nanoid()}`];
const replacements: Record<string, string> = {};

for (const arg of args) {
  const pivot = arg.indexOf('=');
  if (pivot === -1) continue;

  const key = arg.slice(0, pivot);
  const value = arg.slice(pivot + 1);

  replacements[`import.meta.env.${key}`] = JSON.stringify(value);
}

const bundle = await rolldown({
  input: join(import.meta.dirname, 'app.jsx'),
  transform: { define: replacements }
});

const { output } = await bundle.generate({});
if (output.length) console.log(output[0].code);
