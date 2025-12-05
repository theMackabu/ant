import { sqlite3 } from './sqlite3';
import { alloc, free, read, callback, freeCallback, readPtr, FFIType } from 'ant:ffi';

const dbPtrPtr = alloc(8);
const result = sqlite3.call('sqlite3_open', ':memory:', dbPtrPtr);

if (result !== 0) {
  console.log('Failed to open database');
  free(dbPtrPtr);
} else {
  const db = read(dbPtrPtr, FFIType.pointer);
  free(dbPtrPtr);

  sqlite3.call('sqlite3_exec', db, 'CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)', 0, 0, 0);
  sqlite3.call('sqlite3_exec', db, "INSERT INTO users (name, age) VALUES ('Alice', 30)", 0, 0, 0);
  sqlite3.call('sqlite3_exec', db, "INSERT INTO users (name, age) VALUES ('Bob', 25)", 0, 0, 0);
  sqlite3.call('sqlite3_exec', db, "INSERT INTO users (name, age) VALUES ('Charlie', 35)", 0, 0, 0);

  console.log('\nQuerying users with callback:');

  const rowCallback = callback(
    function (_, argc, argv, colNames) {
      let row = '';
      for (let i = 0; i < argc; i++) {
        const colNamePtr = readPtr(colNames + i * 8, FFIType.pointer);
        const valuePtr = readPtr(argv + i * 8, FFIType.pointer);
        const colName = colNamePtr ? readPtr(colNamePtr, FFIType.string) : 'NULL';
        const value = valuePtr ? readPtr(valuePtr, FFIType.string) : 'NULL';
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

  const execResult = sqlite3.call('sqlite3_exec', db, 'SELECT * FROM users', rowCallback, 0, 0);

  if (execResult !== 0) {
    console.log(`Query error: ${sqlite3.call('sqlite3_errmsg', db)}`);
  }

  freeCallback(rowCallback);
  sqlite3.call('sqlite3_close', db);
  console.log('\nDatabase closed.');
}
