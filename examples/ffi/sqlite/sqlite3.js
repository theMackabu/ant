import { dlopen, suffix, FFIType } from 'ant:ffi';

export const sqlite3 = dlopen(`libsqlite3.${suffix}`);

sqlite3.define('sqlite3_libversion', {
  args: [],
  returns: FFIType.string
});

sqlite3.define('sqlite3_open', {
  args: [FFIType.string, FFIType.pointer],
  returns: FFIType.int
});

sqlite3.define('sqlite3_close', {
  args: [FFIType.pointer],
  returns: FFIType.int
});

sqlite3.define('sqlite3_errmsg', {
  args: [FFIType.pointer],
  returns: FFIType.string
});

sqlite3.define('sqlite3_exec', {
  args: [FFIType.pointer, FFIType.string, FFIType.pointer, FFIType.pointer, FFIType.pointer],
  returns: FFIType.int
});

console.log(`SQLite version: ${sqlite3.call('sqlite3_libversion')}`);
