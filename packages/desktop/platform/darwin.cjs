'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const common = require('./common.cjs');

const FRAMEWORK = 'Chromium Embedded Framework.framework';

function layout(root) {
  return {
    executable: path.join(root, 'Ant Desktop.app', 'Contents', 'MacOS', 'Ant Desktop'),
    frameworks: path.join(root, 'Ant Desktop.app', 'Contents', 'Frameworks'),
    library: path.join(root, 'Ant Desktop.app', 'Contents', 'Frameworks', 'libantdesktop.dylib')
  };
}

function sharedRuntime(cefVersion) {
  return common.sharedRuntime(path.join(os.homedir(), 'Library', 'Application Support', 'Ant Desktop', 'CEF', cefVersion));
}

function xml(value) {
  return String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;').replaceAll("'", '&apos;');
}

function plistValue(value, depth = 0) {
  const indent = '  '.repeat(depth);
  if (typeof value === 'string') return `${indent}<string>${xml(value)}</string>`;
  if (typeof value === 'boolean') return `${indent}<${value ? 'true' : 'false'}/>`;
  if (typeof value === 'number' && Number.isFinite(value)) {
    return `${indent}<${Number.isInteger(value) ? 'integer' : 'real'}>${value}</${Number.isInteger(value) ? 'integer' : 'real'}>`;
  }
  if (Array.isArray(value)) {
    const entries = value.map(entry => plistValue(entry, depth + 1)).join('\n');
    return entries ? `${indent}<array>\n${entries}\n${indent}</array>` : `${indent}<array/>`;
  }
  if (value && typeof value === 'object') {
    const entries = Object.entries(value)
      .map(([key, entry]) => `${'  '.repeat(depth + 1)}<key>${xml(key)}</key>\n${plistValue(entry, depth + 1)}`)
      .join('\n');
    return entries ? `${indent}<dict>\n${entries}\n${indent}</dict>` : `${indent}<dict/>`;
  }
  throw new TypeError(`Unsupported property-list value: ${value}`);
}

function propertyList(value) {
  return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
${plistValue(value)}
</plist>
`;
}

function isEnglishFrameworkResource(candidate) {
  const name = path.basename(candidate);
  return !name.endsWith('.lproj') || name === 'en.lproj';
}

function infoPlist({ name, executable, identifier, version, entry, icon, background }) {
  return propertyList({
    CFBundleDevelopmentRegion: 'English',
    CFBundleDisplayName: name,
    CFBundleExecutable: executable,
    CFBundleIdentifier: identifier,
    CFBundleInfoDictionaryVersion: '6.0',
    ...(icon ? { CFBundleIconFile: icon } : {}),
    CFBundleName: name,
    CFBundlePackageType: 'APPL',
    CFBundleShortVersionString: version,
    CFBundleVersion: version,
    LSMinimumSystemVersion: '15.0',
    NSHighResolutionCapable: true,
    ...(background ? { LSUIElement: true } : {}),
    ...(entry ? { AntDesktopEntry: entry } : {})
  });
}

function packageApp(layout, entry, options = {}) {
  const { appRoot, sourceEntry } = common.resolveApplication(entry, options);
  const name = common.applicationName(options.name || path.basename(sourceEntry, path.extname(sourceEntry)));
  const identifier = common.bundleIdentifier(name, options.identifier);
  const version = options.version || '1.0.0';
  const requestedOutput = path.resolve(options.out || path.join(process.cwd(), 'dist', `${name}.app`));
  const output = requestedOutput.endsWith('.app') ? requestedOutput : `${requestedOutput}.app`;
  if (fs.existsSync(output) && !options.overwrite) {
    throw new Error(`Output already exists: ${output} (pass --overwrite to replace it)`);
  }
  const sourceIcon = common.applicationIcon(options.icon, '.icns', 'macOS');
  const icon = sourceIcon && `${name}.icns`;

  common.buildRenderer(options.rendererBuildCommand, appRoot);
  const temporary = `${output}.tmp-${process.pid}-${Date.now()}`;
  const contents = path.join(temporary, 'Contents');
  const macos = path.join(contents, 'MacOS');
  const frameworks = path.join(contents, 'Frameworks');
  const resources = path.join(contents, 'Resources');
  const included = common.includedSources(appRoot, options.include || [], [output, temporary]);
  const bundledEntry = common.bundledEntryPath(included, sourceEntry);

  common.replaceOutput(temporary, output, () => {
    fs.mkdirSync(macos, { recursive: true });
    fs.mkdirSync(resources, { recursive: true });
    fs.copyFileSync(layout.executable, path.join(macos, name));
    fs.chmodSync(path.join(macos, name), 0o755);
    common.copyTree(layout.frameworks, frameworks, []);
    if (options.bundleRuntime) {
      const runtime = path.join(sharedRuntime(layout.lock.cefVersion), FRAMEWORK);
      common.copyTree(runtime, path.join(frameworks, FRAMEWORK), [], isEnglishFrameworkResource);
    }
    common.copyIncluded(included, path.join(resources, 'app'));
    if (sourceIcon) fs.copyFileSync(sourceIcon, path.join(resources, icon));
    fs.writeFileSync(
      path.join(contents, 'Info.plist'),
      infoPlist({ name, executable: name, identifier, version, entry: path.posix.join('app', bundledEntry), icon })
    );
    fs.writeFileSync(path.join(contents, 'PkgInfo'), 'APPL????');
  });

  return { format: 'app', path: output, platform: `darwin-${process.arch}` };
}

function createDevApp(layout, entry, options = {}) {
  const { appRoot, sourceEntry } = common.resolveApplication(entry, options);
  const name = common.applicationName(options.name || path.basename(appRoot));
  const identifier = common.bundleIdentifier(name, options.identifier);
  const key = crypto.createHash('sha256').update(`${appRoot}\0${name}\0${identifier}`).digest('hex').slice(0, 12);
  const output = path.join(options.cacheDir || path.join(os.homedir(), 'Library', 'Caches', 'ant-desktop'), 'dev', key, `${name}.app`);
  const contents = path.join(output, 'Contents');
  const macos = path.join(contents, 'MacOS');
  const resources = path.join(contents, 'Resources');
  const executable = path.join(macos, name);
  const sourceIcon = common.applicationIcon(options.icon, '.icns', 'macOS');
  const icon = sourceIcon && `${name}.icns`;

  fs.mkdirSync(macos, { recursive: true });
  fs.mkdirSync(resources, { recursive: true });
  common.cloneFile(layout.executable, executable);
  common.replaceSymlink(layout.frameworks, path.join(contents, 'Frameworks'), 'dir');
  common.replaceSymlink(appRoot, path.join(resources, 'app'), 'dir');
  if (sourceIcon) common.replaceSymlink(sourceIcon, path.join(resources, icon), 'file');
  fs.writeFileSync(
    path.join(contents, 'Info.plist'),
    infoPlist({
      name,
      executable: name,
      identifier,
      version: options.version || '0.0.0-dev',
      icon,
      entry: path.posix.join('app', path.relative(appRoot, sourceEntry).split(path.sep).join('/'))
    })
  );
  fs.writeFileSync(path.join(contents, 'PkgInfo'), 'APPL????');
  return { appRoot, executable, name, output };
}

module.exports = { createDevApp, infoPlist, layout, packageApp };
