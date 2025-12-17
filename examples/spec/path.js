import { test, summary } from './helpers.js';
import path from 'path';

console.log('Path Tests\n');

test('path.join', path.join('a', 'b', 'c'), 'a/b/c');
test('path.join with .', path.join('a', '.', 'b'), 'a/b');
test('path.join with ..', path.join('a', 'b', '..', 'c'), 'a/c');

test('path.resolve absolute', path.resolve('/a', 'b'), '/a/b');
test('path.resolve relative', typeof path.resolve('a', 'b'), 'string');

test('path.dirname', path.dirname('/a/b/c.txt'), '/a/b');
test('path.basename', path.basename('/a/b/c.txt'), 'c.txt');
test('path.basename with ext', path.basename('/a/b/c.txt', '.txt'), 'c');
test('path.extname', path.extname('/a/b/c.txt'), '.txt');
test('path.extname no ext', path.extname('/a/b/c'), '');

test('path.isAbsolute true', path.isAbsolute('/a/b'), true);
test('path.isAbsolute false', path.isAbsolute('a/b'), false);

test('path.normalize', path.normalize('/a/b/../c/./d'), '/a/c/d');

const parsed = path.parse('/home/user/file.txt');
test('path.parse root', parsed.root, '/');
test('path.parse dir', parsed.dir, '/home/user');
test('path.parse base', parsed.base, 'file.txt');
test('path.parse name', parsed.name, 'file');
test('path.parse ext', parsed.ext, '.txt');

test('path.format', path.format({ dir: '/home/user', base: 'file.txt' }), '/home/user/file.txt');

test('path.sep', path.sep, '/');
test('path.delimiter', path.delimiter, ':');

summary();
