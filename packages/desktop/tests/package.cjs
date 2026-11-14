'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { packageApp } = require('../packaging/npm/darwin-arm64/package.cjs');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-desktop-package-'));
try {
  const appRoot = path.join(root, 'source');
  const entry = path.join(appRoot, 'main.js');
  const executable = path.join(root, 'ant-desktop');
  const frameworks = path.join(root, 'Frameworks');
  fs.mkdirSync(appRoot, { recursive: true });
  fs.mkdirSync(frameworks, { recursive: true });
  fs.writeFileSync(entry, 'console.log("packaged");\n');
  fs.writeFileSync(path.join(appRoot, 'page.html'), '<h1>Packaged</h1>\n');
  const extra = path.join(appRoot, 'assets', 'model.bin');
  fs.mkdirSync(path.dirname(extra), { recursive: true });
  fs.writeFileSync(extra, 'model-data\n');
  fs.writeFileSync(executable, '#!/bin/sh\n');
  const frameworkResources = path.join(frameworks, 'Fake.framework', 'Versions', 'A', 'Resources');
  fs.mkdirSync(path.join(frameworkResources, 'en.lproj'), { recursive: true });
  fs.mkdirSync(path.join(frameworkResources, 'fr.lproj'), { recursive: true });
  fs.writeFileSync(path.join(frameworkResources, 'en.lproj', 'locale.pak'), 'english\n');
  fs.writeFileSync(path.join(frameworkResources, 'fr.lproj', 'locale.pak'), 'french\n');
  fs.chmodSync(executable, 0o755);

  const output = path.join(root, 'dist', 'PackageTest.app');
  const result = packageApp({ executable, frameworks }, entry, {
    appDir: appRoot,
    identifier: 'com.antjs.package-test',
    name: 'PackageTest',
    out: output,
    version: '1.2.3',
    rendererBuildCommand: '/bin/mkdir -p renderer/dist && /usr/bin/touch renderer/dist/index.html',
    include: ['main.js', 'renderer/dist/**', 'assets/*']
  });

  assert.equal(result.format, 'app');
  assert.equal(result.path, output);
  assert.ok(fs.existsSync(path.join(appRoot, 'renderer', 'dist', 'index.html')));
  assert.ok(fs.statSync(path.join(output, 'Contents', 'MacOS', 'PackageTest')).mode & 0o100);
  const bundled = path.join(output, 'Contents', 'Resources', 'app');
  assert.equal(fs.readFileSync(path.join(bundled, 'main.js'), 'utf8'), 'console.log("packaged");\n');
  assert.ok(fs.existsSync(path.join(bundled, 'renderer', 'dist', 'index.html')));
  assert.equal(fs.readFileSync(path.join(bundled, 'assets', 'model.bin'), 'utf8'), 'model-data\n');
  assert.ok(!fs.existsSync(path.join(bundled, 'page.html')));
  assert.ok(fs.existsSync(path.join(output, 'Contents', 'Frameworks', 'Fake.framework', 'Versions', 'A', 'Resources', 'fr.lproj')));
  const plist = fs.readFileSync(path.join(output, 'Contents', 'Info.plist'), 'utf8');
  assert.match(plist, /<string>com\.antjs\.package-test<\/string>/);
  assert.match(plist, /<key>CFBundleDevelopmentRegion<\/key>\s*<string>English<\/string>/);
  assert.match(plist, /<key>AntDesktopEntry<\/key>\s*<string>app\/main\.js<\/string>/);
  assert.doesNotMatch(plist, /AntDesktopArchive/);
  assert.throws(() => packageApp({ executable, frameworks }, entry, { appDir: appRoot, out: output }), /Output already exists/);
  console.log('desktop-package-smoke-ok');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
