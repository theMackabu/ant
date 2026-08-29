const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-directory-main-'));
const ant = path.resolve(process.execPath);

function writePackage(name, packageJson, files) {
  const directory = path.join(tmpRoot, name);
  fs.mkdirSync(directory, { recursive: true });
  fs.writeFileSync(path.join(directory, 'package.json'), `${JSON.stringify(packageJson)}\n`);

  for (const [relativePath, source] of Object.entries(files)) {
    const filename = path.join(directory, relativePath);
    fs.mkdirSync(path.dirname(filename), { recursive: true });
    fs.writeFileSync(filename, source);
  }
}

function runPackage(name, expected) {
  const result = spawnSync(ant, [name], {
    cwd: tmpRoot,
    encoding: 'utf8',
  });

  if (result.error) throw result.error;
  assert(
    result.status === 0,
    `${name} should exit 0, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  );
  assert(
    result.stdout === `${expected}\n`,
    `${name} should print ${JSON.stringify(expected)}, got ${JSON.stringify(result.stdout)}`
  );
}

try {
  writePackage(
    'main-before-index',
    { main: 'src/main.cjs' },
    {
      'src/main.cjs': 'console.log("package main");\n',
      'index.cjs': 'console.log("package index");\n',
    }
  );
  writePackage(
    'extensionless-main',
    { main: 'src/main' },
    { 'src/main.cjs': 'console.log("extensionless main");\n' }
  );
  writePackage(
    'directory-main',
    { main: 'src' },
    { 'src/index.cjs': 'console.log("directory main index");\n' }
  );
  writePackage(
    'missing-main-target',
    { main: 'missing.cjs' },
    { 'index.cjs': 'console.log("missing main fallback");\n' }
  );
  writePackage(
    'invalid-main',
    { main: 42 },
    { 'index.cjs': 'console.log("invalid main fallback");\n' }
  );

  runPackage('main-before-index', 'package main');
  runPackage('extensionless-main', 'extensionless main');
  runPackage('directory-main', 'directory main index');
  runPackage('missing-main-target', 'missing main fallback');
  runPackage('invalid-main', 'invalid main fallback');

  console.log('directory package main entrypoint test passed');
} finally {
  fs.rmSync(tmpRoot, { recursive: true, force: true });
}
