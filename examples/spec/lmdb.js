import { test, summary } from './helpers.js';
import lmdb from 'ant:lmdb';
import fs from 'ant:fs';

console.log('LMDB Tests\n');

const dbPath = `ant-spec-lmdb-${Date.now()}-${Math.floor(Math.random() * 1e6)}.mdb`;

test('lmdb.version is string', typeof lmdb.version, 'string');
test('lmdb.open exists', typeof lmdb.open, 'function');
test('lmdb.constants exists', typeof lmdb.constants, 'object');

const env = lmdb.open(dbPath, {
  noSubdir: true,
  mapSize: 4 * 1024 * 1024,
  maxDbs: 8
});
test('env open', typeof env.openDB, 'function');

const db = env.openDB('main', { create: true });
test('db open', typeof db.get, 'function');

db.put('hello', 'world');
test('db.getString', db.getString('hello'), 'world');
test('db.get utf8', db.get('hello', 'utf8'), 'world');

const bytesIn = new Uint8Array([1, 2, 3, 255]);
db.put('bytes', bytesIn);
const bytesOut = db.getBytes('bytes');
test('db.get bytes is Uint8Array', bytesOut instanceof Uint8Array, true);
test('db.get bytes length', bytesOut.length, 4);
test('db.get bytes[3]', bytesOut[3], 255);
const bytesOutViaGet = db.get('bytes', 'bytes');
test('db.get(bytes) returns Uint8Array', bytesOutViaGet instanceof Uint8Array, true);
test('db.get(bytes) value preserved', bytesOutViaGet[3], 255);

const tx = env.beginTxn();
tx.put(db, 'tx-key', 'tx-value');
tx.commit();
test('txn commit', db.getString('tx-key'), 'tx-value');

const ro = env.beginTxn({ readOnly: true });
test('ro txn read', ro.getString(db, 'tx-key'), 'tx-value');
test('ro txn get utf8', ro.get(db, 'tx-key', 'utf8'), 'tx-value');
ro.abort();

test('db.del returns true', db.del('hello'), true);
test('db.get missing returns undefined', db.get('hello'), undefined);

const stat = env.stat();
const info = env.info();
test('env.stat entries number', typeof stat.entries, 'number');
test('env.info mapSize number', typeof info.mapSize, 'number');

db.close();
env.close();

if (fs.existsSync(dbPath)) fs.unlinkSync(dbPath);
if (fs.existsSync(`${dbPath}-lock`)) fs.unlinkSync(`${dbPath}-lock`);

test('db file cleaned up', fs.existsSync(dbPath), false);
test('db lock file cleaned up', fs.existsSync(`${dbPath}-lock`), false);

summary();
