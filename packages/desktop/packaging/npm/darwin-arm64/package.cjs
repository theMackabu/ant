'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { buildRenderer } = require('./renderer.cjs');

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

function applicationName(value) {
  if (!value.trim() || /[/:\\]/.test(value) || value.includes('\0')) {
    throw new Error('Application name must not be empty or contain /, :, or NUL');
  }
  return value.trim();
}

function bundleIdentifier(name, configured) {
  const fallback = name.toLowerCase().replace(/[^a-z0-9]+/g, '-');
  const value = configured || `org.antjs.${fallback}`;
  if (!/^[A-Za-z0-9.-]+$/.test(value)) {
    throw new Error('Application identifier may contain only letters, numbers, dots, and hyphens');
  }
  return value;
}

function isInside(parent, child) {
  const relative = path.relative(parent, child);
  return relative === '' || (!relative.startsWith('..') && !path.isAbsolute(relative));
}

function copyTree(source, destination, excluded, include = () => true) {
  fs.cpSync(source, destination, {
    recursive: true,
    dereference: false,
    verbatimSymlinks: true,
    filter(candidate) {
      const resolved = path.resolve(candidate);
      return include(candidate) && !excluded.some(value => isInside(value, resolved));
    }
  });
}

const ARCHIVE_MAGIC = Buffer.from('ANTAPP01');

function writeUnsigned(buffer, offset, value, bytes) {
  let remaining = value;
  for (let index = 0; index < bytes; index++) {
    buffer[offset + index] = remaining % 256;
    remaining = Math.floor(remaining / 256);
  }
}

function includeExpression(value) {
  if (typeof value !== 'string' || !value.trim() || path.isAbsolute(value)) {
    throw new Error(`Include pattern must be a relative glob: ${value}`);
  }
  const normalized = value.replaceAll('\\', '/');
  if (normalized.split('/').some(part => !part || part === '.' || part === '..')) {
    throw new Error(`Include pattern must stay inside the application directory: ${value}`);
  }
  let expression = '^';
  for (let index = 0; index < normalized.length; index++) {
    const character = normalized[index];
    if (character === '*' && normalized[index + 1] === '*') {
      index++;
      if (normalized[index + 1] === '/') {
        index++;
        expression += '(?:.*/)?';
      } else {
        expression += '.*';
      }
    } else if (character === '*') {
      expression += '[^/]*';
    } else if (character === '?') {
      expression += '[^/]';
    } else {
      expression += character.replace(/[|\\{}()[\]^$+?.]/g, '\\$&');
    }
  }
  return { pattern: value, expression: new RegExp(`${expression}$`) };
}

function matchesInclude(relative, patterns) {
  const normalized = relative.split(path.sep).join('/');
  return patterns.some(value => includeExpression(value).expression.test(normalized));
}

function includedSources(root, values, excluded = []) {
  if (!Array.isArray(values)) throw new TypeError('include must be an array');
  const patterns = values.map(includeExpression);
  const matches = new Array(patterns.length).fill(0);
  const included = [];
  const visit = current => {
    const resolved = path.resolve(current);
    if (excluded.some(value => isInside(value, resolved))) return;
    const stat = fs.lstatSync(current);
    if (stat.isDirectory()) {
      for (const name of fs.readdirSync(current).sort()) visit(path.join(current, name));
      return;
    }
    const destination = path.relative(root, current).split(path.sep).join('/');
    let selected = false;
    patterns.forEach((pattern, index) => {
      if (!pattern.expression.test(destination)) return;
      matches[index]++;
      selected = true;
    });
    if (selected) included.push({ source: current, destination, stat });
  };
  visit(root);
  const missing = patterns.find((_pattern, index) => matches[index] === 0);
  if (missing) throw new Error(`Include pattern matched no files: ${missing.pattern}`);
  return included;
}

function archiveEntries(included, excluded = []) {
  const entries = [];
  const destinations = new Set();
  const visit = (current, destination) => {
    const resolved = path.resolve(current);
    if (excluded.some(value => isInside(value, resolved))) return;
    const stat = fs.lstatSync(current);
    if (stat.isDirectory()) {
      for (const name of fs.readdirSync(current).sort()) {
        visit(path.join(current, name), destination ? `${destination}/${name}` : name);
      }
      return;
    }
    if (!stat.isFile() && !stat.isSymbolicLink()) {
      throw new Error(`Application archive cannot contain this file type: ${current}`);
    }
    if (destinations.has(destination)) {
      throw new Error(`Multiple bundle entries target ${destination}`);
    }
    destinations.add(destination);
    entries.push({ current, relative: destination, stat });
  };
  for (const entry of included) visit(entry.source, entry.destination);
  return entries;
}

function writeApplicationArchive(included, destination, excluded) {
  const entries = archiveEntries(included, excluded);
  const fd = fs.openSync(destination, 'w');
  try {
    const count = Buffer.alloc(4);
    writeUnsigned(count, 0, entries.length, 4);
    fs.writeSync(fd, ARCHIVE_MAGIC);
    fs.writeSync(fd, count);
    for (const entry of entries) {
      const name = Buffer.from(entry.relative);
      const symbolic = entry.stat.isSymbolicLink();
      const data = symbolic ? Buffer.from(fs.readlinkSync(entry.current)) : fs.readFileSync(entry.current);
      const header = Buffer.alloc(20);
      writeUnsigned(header, 0, name.length, 4);
      writeUnsigned(header, 4, data.length, 8);
      writeUnsigned(header, 12, entry.stat.mode & 0o777, 4);
      header[16] = symbolic ? 2 : 1;
      fs.writeSync(fd, header);
      fs.writeSync(fd, name);
      fs.writeSync(fd, data);
    }
  } finally {
    fs.closeSync(fd);
  }
}

function bundledEntryPath(included, sourceEntry) {
  const entry = included.find(value => path.resolve(value.source) === sourceEntry);
  if (!entry) {
    throw new Error('Application entry is not included');
  }
  return entry.destination;
}

function isEnglishFrameworkResource(candidate) {
  const name = path.basename(candidate);
  return !name.endsWith('.lproj') || name === 'en.lproj';
}

function infoPlist({ name, executable, identifier, version, entry, icon, archive }) {
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
    AntDesktopEntry: entry,
    ...(archive ? { AntDesktopArchive: archive } : {})
  });
}

function packageApp(layout, entry, options = {}) {
  const sourceEntry = path.resolve(entry);
  if (!fs.statSync(sourceEntry, { throwIfNoEntry: false })?.isFile()) {
    throw new Error(`Application entry does not exist: ${sourceEntry}`);
  }
  const appRoot = path.resolve(options.appDir || path.dirname(sourceEntry));
  if (!fs.statSync(appRoot, { throwIfNoEntry: false })?.isDirectory()) {
    throw new Error(`Application directory does not exist: ${appRoot}`);
  }
  if (!isInside(appRoot, sourceEntry)) {
    throw new Error('Application entry must be inside --app-dir');
  }
  const name = applicationName(options.name || path.basename(sourceEntry, path.extname(sourceEntry)));
  const executable = name;
  const identifier = bundleIdentifier(name, options.identifier);
  const version = options.version || '1.0.0';
  const requestedOutput = path.resolve(options.out || path.join(process.cwd(), 'dist', `${name}.app`));
  const output = requestedOutput.endsWith('.app') ? requestedOutput : `${requestedOutput}.app`;
  if (fs.existsSync(output) && !options.overwrite) {
    throw new Error(`Output already exists: ${output} (pass --overwrite to replace it)`);
  }

  let icon;
  if (options.icon) {
    const sourceIcon = path.resolve(options.icon);
    if (path.extname(sourceIcon).toLowerCase() !== '.icns' || !fs.statSync(sourceIcon, { throwIfNoEntry: false })?.isFile()) {
      throw new Error('macOS application icon must be an existing .icns file');
    }
    icon = `${name}.icns`;
  }

  buildRenderer(options.rendererBuildCommand, appRoot);
  const temporary = `${output}.tmp-${process.pid}-${Date.now()}`;
  const contents = path.join(temporary, 'Contents');
  const macos = path.join(contents, 'MacOS');
  const frameworks = path.join(contents, 'Frameworks');
  const resources = path.join(contents, 'Resources');
  const archive = 'app.ant';
  const excluded = [output, temporary];
  const included = includedSources(appRoot, options.include || [], excluded);
  const bundledEntry = bundledEntryPath(included, sourceEntry);

  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.rmSync(temporary, { recursive: true, force: true });
  try {
    fs.mkdirSync(macos, { recursive: true });
    fs.mkdirSync(frameworks, { recursive: true });
    fs.mkdirSync(resources, { recursive: true });
    fs.copyFileSync(layout.executable, path.join(macos, executable));
    fs.chmodSync(path.join(macos, executable), 0o755);
    for (const entry of fs.readdirSync(layout.frameworks)) {
      copyTree(path.join(layout.frameworks, entry), path.join(frameworks, entry), [], isEnglishFrameworkResource);
    }
    writeApplicationArchive(included, path.join(resources, archive), excluded);
    if (options.icon) fs.copyFileSync(path.resolve(options.icon), path.join(resources, icon));
    fs.writeFileSync(
      path.join(contents, 'Info.plist'),
      infoPlist({
        name,
        executable,
        identifier,
        version,
        entry: bundledEntry,
        icon,
        archive
      })
    );
    fs.writeFileSync(path.join(contents, 'PkgInfo'), 'APPL????');
    if (fs.existsSync(output)) fs.rmSync(output, { recursive: true, force: true });
    fs.renameSync(temporary, output);
  } catch (error) {
    fs.rmSync(temporary, { recursive: true, force: true });
    throw error;
  }

  return { format: 'app', path: output, platform: 'darwin-arm64' };
}

module.exports = {
  bundleIdentifier,
  infoPlist,
  isInside,
  packageApp,
  applicationName,
  includedSources,
  bundledEntryPath,
  matchesInclude,
  writeApplicationArchive
};
