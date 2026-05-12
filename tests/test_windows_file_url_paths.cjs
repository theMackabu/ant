const filename = import.meta.filename;
const url = import.meta.url;

if (!url.startsWith('file://')) {
  throw new Error('expected file URL for import.meta.url, got ' + url);
}

const isWindowsPath =
  filename.length >= 3 &&
  filename[1] === ':' &&
  (filename[2] === '\\' || filename[2] === '/');

if (isWindowsPath) {
  const drive = filename[0];
  if (!url.startsWith('file:///' + drive + ':/')) {
    throw new Error('expected Windows file URL to use file:///C:/ form, got ' + url);
  }
  if (url.startsWith('file://' + drive + ':')) {
    throw new Error('Windows file URL must not put the drive in the authority: ' + url);
  }
}

const resolved = import.meta.resolve('./test_import_meta.cjs');
if (!resolved.startsWith('file://')) {
  throw new Error('expected import.meta.resolve to return a file URL, got ' + resolved);
}

if (isWindowsPath && !resolved.startsWith('file:///')) {
  throw new Error('expected resolved Windows path to use file:/// form, got ' + resolved);
}

const resolvedSelf = import.meta.resolve(url);
if (resolvedSelf !== url) {
  throw new Error('expected resolving import.meta.url to round-trip, got ' + resolvedSelf);
}

console.log('windows file URL path regression ok');
