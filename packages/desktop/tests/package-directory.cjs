'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { createDevApp, layout, packageApp } = require('../platform/directory.cjs');

const files = {
  linux: { launcher: 'ant-desktop', library: 'libantdesktop.so', executable: 'PackageTest' },
  windows: { launcher: 'ant-desktop.exe', library: 'antdesktop.dll', executable: 'PackageTest.exe' }
};

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-desktop-directory-'));
try {
  const appRoot = path.join(root, 'source');
  const entry = path.join(appRoot, 'main.js');
  const icon = path.join(root, 'icon.png');
  fs.mkdirSync(path.join(appRoot, 'assets'), { recursive: true });
  fs.writeFileSync(entry, 'console.log("packaged");\n');
  fs.writeFileSync(path.join(appRoot, 'page.html'), '<h1>Packaged</h1>\n');
  fs.writeFileSync(path.join(appRoot, 'assets', 'model.bin'), 'model-data\n');
  fs.writeFileSync(icon, 'png');

  for (const [system, names] of Object.entries(files)) {
    const payload = path.join(root, system, 'payload');
    fs.mkdirSync(path.join(payload, 'ant-desktop'), { recursive: true });
    fs.writeFileSync(path.join(payload, 'ant-desktop', names.launcher), 'launcher');
    fs.writeFileSync(path.join(payload, 'ant-desktop', names.library), 'library');
    const runtime = layout(payload, system);
    assert.equal(path.basename(runtime.executable), names.launcher);
    assert.equal(path.basename(runtime.library), names.library);

    const output = path.join(root, system, 'dist', 'PackageTest');
    const options = {
      appDir: appRoot,
      icon,
      identifier: 'com.antjs.package-test',
      name: 'PackageTest',
      out: output,
      version: '1.2.3',
      include: ['main.js', 'assets/*']
    };
    const result = packageApp(runtime, entry, options);
    assert.deepEqual(result, { format: 'directory', path: output, platform: `${system}-${process.arch}` });
    assert.equal(fs.readFileSync(path.join(output, names.executable), 'utf8'), 'launcher');
    assert.ok(fs.statSync(path.join(output, names.executable)).mode & 0o100);
    assert.equal(fs.readFileSync(path.join(output, names.library), 'utf8'), 'library');
    const resources = path.join(output, 'resources');
    assert.equal(
      fs.readFileSync(path.join(resources, 'app.info'), 'utf8'),
      'AntDesktopName=PackageTest\nAntDesktopIdentifier=com.antjs.package-test\nAntDesktopVersion=1.2.3\n' +
        'AntDesktopEntry=app/main.js\nAntDesktopIcon=icon.png\n'
    );
    assert.equal(fs.readFileSync(path.join(resources, 'icon.png'), 'utf8'), 'png');
    assert.equal(fs.readFileSync(path.join(resources, 'app', 'assets', 'model.bin'), 'utf8'), 'model-data\n');
    assert.ok(!fs.existsSync(path.join(resources, 'app', 'page.html')));
    assert.throws(() => packageApp(runtime, entry, options), /Output already exists/);
    assert.throws(() => packageApp(runtime, entry, { ...options, overwrite: true, icon: entry }), /existing \.png file/);

    const development = createDevApp(runtime, entry, { cacheDir: path.join(root, system, 'cache'), name: 'Dev Example' });
    assert.equal(development.name, 'Dev Example');
    assert.equal(fs.readFileSync(development.executable, 'utf8'), 'launcher');
    assert.equal(fs.readFileSync(path.join(development.output, names.library), 'utf8'), 'library');
    assert.equal(fs.realpathSync(path.join(development.output, 'resources', 'app')), fs.realpathSync(appRoot));
    assert.match(fs.readFileSync(path.join(development.output, 'resources', 'app.info'), 'utf8'), /^AntDesktopEntry=app\/main\.js$/m);
  }
  console.log('desktop-package-directory-ok');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
