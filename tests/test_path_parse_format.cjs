const { posix } = require('node:path');
const { win32 } = require('node:path');

function assertEqual(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

const parsedBasename = posix.parse('file.txt');
assertEqual(parsedBasename.dir, '', 'basename-only parse dir');
assertEqual(posix.format(parsedBasename), 'file.txt', 'format(parse(path))');
assertEqual(
  posix.format({ dir: '', root: '/', base: 'file' }),
  '/file',
  'empty dir should fall back to root'
);
assertEqual(
  posix.format({ dir: '/x', base: '', name: 'file', ext: '.js' }),
  '/x/file.js',
  'empty base should fall back to name and ext'
);
assertEqual(
  win32.format({ dir: '', root: 'C:\\', base: 'file' }),
  'C:\\file',
  'empty win32 dir should fall back to root'
);
assertEqual(
  win32.format({ dir: 'D:\\work', root: 'C:\\', base: 'file' }),
  'D:\\work\\file',
  'non-empty win32 dir should override root'
);

console.log('path parse/format empty fields ok');
