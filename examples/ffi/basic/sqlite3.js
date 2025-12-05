import { dlopen, suffix, FFIType } from 'ant:ffi';

const sqlite3 = dlopen(`libsqlite3.${suffix}`);

sqlite3.define('sqlite3_libversion', {
  args: [],
  returns: FFIType.string
});

console.log(`SQLite 3 version: ${sqlite3.call('sqlite3_libversion')}`);
