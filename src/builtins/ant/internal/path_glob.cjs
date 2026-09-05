const { minimatch } = require('minimatch');
const { validateString, isWindows } = require('ant:internal/path_helpers');

const kEmptyObject = Object.freeze({ __proto__: null });
const isMacOS = process.platform === 'darwin';

exports.matchGlobPattern = function (path, pattern, windows = isWindows) {
  validateString(path, 'path');
  validateString(pattern, 'pattern');

  return minimatch(path, pattern, {
    kEmptyObject,
    nocase: isMacOS || isWindows,
    windowsPathsNoEscape: true,
    nonegate: true,
    nocomment: true,
    optimizationLevel: 2,
    platform: windows ? 'win32' : 'posix',
    nocaseMagicOnly: true
  });
};
