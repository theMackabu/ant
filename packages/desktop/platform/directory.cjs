'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const common = require('./common.cjs');

const hostSystem = process.platform === 'win32' ? 'windows' : 'linux';
const systems = {
  linux: { launcher: 'ant-desktop', library: 'libantdesktop.so', suffix: '', link: 'dir' },
  windows: { launcher: 'ant-desktop.exe', library: 'antdesktop.dll', suffix: '.exe', link: 'junction' }
};

function layout(root, system = hostSystem) {
  const directory = path.join(root, 'ant-desktop');
  return {
    system,
    executable: path.join(directory, systems[system].launcher),
    library: path.join(directory, systems[system].library)
  };
}

function dataRoot(system, variable, fallback) {
  if (system === 'windows') return path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'), 'Ant Desktop');
  return path.join(process.env[variable] || path.join(os.homedir(), fallback), 'ant-desktop');
}

function sharedRuntime(system, cefVersion) {
  const root = dataRoot(system, 'XDG_DATA_HOME', path.join('.local', 'share'));
  return common.sharedRuntime(path.join(root, system === 'windows' ? 'CEF' : 'cef', cefVersion));
}

function applicationInfo(values) {
  return Object.entries(values)
    .filter(([, value]) => value !== undefined)
    .map(([key, value]) => `${key}=${value}\n`)
    .join('');
}

function writeApplication(directory, layout, application, copy) {
  const { launcher, library, suffix } = systems[layout.system];
  const resources = path.join(directory, 'resources');
  const executable = path.join(directory, `${application.name}${suffix}`);
  fs.mkdirSync(resources, { recursive: true });
  copy(layout.executable, executable);
  copy(layout.library, path.join(directory, library));
  fs.writeFileSync(
    path.join(resources, 'app.info'),
    applicationInfo({
      AntDesktopName: application.name,
      AntDesktopIdentifier: application.identifier,
      AntDesktopVersion: application.version,
      AntDesktopEntry: application.entry,
      AntDesktopIcon: application.icon && 'icon.png'
    })
  );
  return { executable, resources, launcher };
}

function copyExecutable(source, destination) {
  fs.copyFileSync(source, destination);
  fs.chmodSync(destination, 0o755);
}

function packageApp(layout, entry, options = {}) {
  const { appRoot, sourceEntry } = common.resolveApplication(entry, options);
  const name = common.applicationName(options.name || path.basename(sourceEntry, path.extname(sourceEntry)));
  const identifier = common.bundleIdentifier(name, options.identifier);
  const output = path.resolve(options.out || path.join(process.cwd(), 'dist', name));
  if (fs.existsSync(output) && !options.overwrite) {
    throw new Error(`Output already exists: ${output} (pass --overwrite to replace it)`);
  }
  const icon = common.applicationIcon(options.icon, '.png', layout.system === 'windows' ? 'Windows' : 'Linux');

  common.buildRenderer(options.rendererBuildCommand, appRoot);
  const temporary = `${output}.tmp-${process.pid}-${Date.now()}`;
  const included = common.includedSources(appRoot, options.include || [], [output, temporary]);
  const bundledEntry = common.bundledEntryPath(included, sourceEntry);

  common.replaceOutput(temporary, output, () => {
    const application = { name, identifier, version: options.version || '1.0.0', entry: path.posix.join('app', bundledEntry), icon };
    const { resources } = writeApplication(temporary, layout, application, copyExecutable);
    if (options.bundleRuntime) common.copyTree(sharedRuntime(layout.system, layout.lock.cefVersion), temporary, []);
    common.copyIncluded(included, path.join(resources, 'app'));
    if (icon) fs.copyFileSync(icon, path.join(resources, 'icon.png'));
  });

  return { format: 'directory', path: output, platform: `${layout.system}-${process.arch}` };
}

function createDevApp(layout, entry, options = {}) {
  const { appRoot, sourceEntry } = common.resolveApplication(entry, options);
  const name = common.applicationName(options.name || path.basename(appRoot));
  const identifier = common.bundleIdentifier(name, options.identifier);
  const icon = common.applicationIcon(options.icon, '.png', layout.system === 'windows' ? 'Windows' : 'Linux');
  const key = crypto.createHash('sha256').update(`${appRoot}\0${name}\0${identifier}`).digest('hex').slice(0, 12);
  const cache = options.cacheDir || dataRoot(layout.system, 'XDG_CACHE_HOME', '.cache');
  const output = path.join(cache, 'dev', key, name);
  const application = {
    name,
    identifier,
    version: options.version || '0.0.0-dev',
    entry: path.posix.join('app', path.relative(appRoot, sourceEntry).split(path.sep).join('/')),
    icon
  };

  fs.mkdirSync(output, { recursive: true });
  const { executable, resources } = writeApplication(output, layout, application, common.cloneFile);
  common.replaceSymlink(appRoot, path.join(resources, 'app'), systems[layout.system].link);
  if (icon) fs.copyFileSync(icon, path.join(resources, 'icon.png'));
  return { appRoot, executable, name, output };
}

module.exports = { applicationInfo, createDevApp, layout, packageApp };
