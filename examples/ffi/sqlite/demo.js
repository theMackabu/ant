import { sqlite3 } from './sqlite3';
import { alloc, callback, FFIType } from 'ant:ffi';

const dbPtrPtr = alloc(8);
const result = sqlite3.call('sqlite3_open', ':memory:', dbPtrPtr);

if (result !== 0) {
  console.log('Failed to open database');
  dbPtrPtr.free();
  process.exit(0);
}

const db = dbPtrPtr.read(FFIType.pointer);
dbPtrPtr.free();

sqlite3.call('sqlite3_exec', db, 'CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)', null, null, null);
sqlite3.call('sqlite3_exec', db, "INSERT INTO users (name, age) VALUES ('Alice', 30)", null, null, null);
sqlite3.call('sqlite3_exec', db, "INSERT INTO users (name, age) VALUES ('Bob', 25)", null, null, null);
sqlite3.call('sqlite3_exec', db, "INSERT INTO users (name, age) VALUES ('Charlie', 35)", null, null, null);

console.log('\nQuerying users with callback:');

const rowCallback = callback(
  function (_, argc, argv, colNames) {
    let row = '';
    for (let i = 0; i < argc; i++) {
      const colNamePtr = colNames.offset(i * 8).read(FFIType.pointer);
      const valuePtr = argv.offset(i * 8).read(FFIType.pointer);
      const colName = colNamePtr ? colNamePtr.read(FFIType.string) : 'NULL';
      const value = valuePtr ? valuePtr.read(FFIType.string) : 'NULL';
      row += `${colName}=${value} `;
    }
    console.log(`  Row: ${row}`);
    return 0;
  },
  {
    args: [FFIType.pointer, FFIType.int, FFIType.pointer, FFIType.pointer],
    returns: FFIType.int
  }
);

const execResult = sqlite3.call('sqlite3_exec', db, 'SELECT * FROM users', rowCallback, null, null);

if (execResult !== 0) {
  console.log(`Query error: ${sqlite3.call('sqlite3_errmsg', db)}`);
}

rowCallback.close();
sqlite3.call('sqlite3_close', db);
console.log('\nDatabase closed.');
