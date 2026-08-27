const examples = [
  ['Basic evaluation', './basic.mjs'],
  ['Host functions', './host-functions.mjs'],
  ['Isolated runtimes', './isolated-runtimes.mjs'],
  ['Rules engine', './rules-engine.mjs'],
  ['Limits and errors', './limits-and-errors.mjs']
];

for (const [name, path] of examples) {
  console.log(`\n--- ${name} ---`);
  await import(path);
}
