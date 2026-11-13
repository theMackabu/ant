import { Database, backup, constants } from 'node:sqlite';
import { channel } from 'node:diagnostics_channel';
import { mkdtempSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const schema = 'CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT)';
const count = db => db.prepare('SELECT count(*) AS total FROM notes').get().total;

console.log('Example 1: Serialize and clone');
const primary = new Database(':memory:');
primary.exec(schema);
primary.exec("INSERT INTO notes (body) VALUES ('first'), ('second')");

const image = primary.serialize();
const clone = new Database(':memory:');
clone.deserialize(image);
console.log(`  ${image.length} bytes -> clone has ${count(clone)} notes`);

console.log('\nExample 2: Sessions and changesets');
const replica = new Database(':memory:');
replica.exec(schema);
replica.exec("INSERT INTO notes (body) VALUES ('first'), ('second')");

const session = primary.createSession({ table: 'notes' });
primary.exec("INSERT INTO notes (body) VALUES ('third')");
primary.exec("UPDATE notes SET body = 'first, edited' WHERE id = 1");

const changeset = session.changeset();
console.log(`  changeset is ${changeset.length} bytes, patchset is ${session.patchset().length} bytes`);
console.log('  applied:', replica.applyChangeset(changeset));
console.log('  replica:', replica.prepare('SELECT body FROM notes ORDER BY id').all().map(note => note.body));

console.log('\nExample 3: Resolving conflicts');
const conflictKinds = Object.fromEntries(
  ['DATA', 'NOTFOUND', 'CONFLICT', 'CONSTRAINT', 'FOREIGN_KEY'].map(kind => [constants[`SQLITE_CHANGESET_${kind}`], kind])
);

const edits = primary.createSession();
replica.exec("UPDATE notes SET body = 'changed on the replica' WHERE id = 2");
primary.exec("UPDATE notes SET body = 'changed on the primary' WHERE id = 2");

const applied = replica.applyChangeset(edits.changeset(), {
  onConflict(kind) {
    console.log('  conflict:', conflictKinds[kind]);
    return constants.SQLITE_CHANGESET_REPLACE;
  }
});
console.log('  applied:', applied);
console.log('  note 2:', replica.prepare('SELECT body FROM notes WHERE id = 2').get().body);
edits.close();
session.close();

console.log('\nExample 4: Online backup');
const dir = mkdtempSync(join(tmpdir(), 'ant-sqlite-demo-'));
const file = join(dir, 'notes.db');

const filler = primary.prepare('INSERT INTO notes (body) VALUES (?)');
for (let i = 0; i < 500; i++) filler.run('x'.repeat(200));

let steps = 0;
const pages = await backup(primary, file, { rate: 8, progress: () => steps++ });
console.log(`  copied ${pages} pages in ${steps + 1} steps (${statSync(file).size} bytes on disk)`);

const restored = new Database(file, { readOnly: true });
console.log('  restored notes:', count(restored));
restored.close();

console.log('\nExample 5: Query tracing');
const queries = channel('sqlite.db.query');
const onQuery = ({ sql, duration }) => console.log(`  ${(duration / 1e6).toFixed(1)} ms  ${sql}`);

queries.subscribe(onQuery);
primary.prepare('SELECT count(*) FROM notes WHERE length(body) > ?').get(100);
primary.prepare('DELETE FROM notes WHERE id > ?').run(3);
queries.unsubscribe(onQuery);

console.log('\nExample 6: Limits');
const guarded = new Database(':memory:', { limits: { sqlLength: 64 } });
try {
  guarded.exec(`SELECT '${'a'.repeat(100)}'`);
} catch (error) {
  console.log('  rejected:', error.message);
}
guarded.limits.sqlLength = Infinity;
console.log('  after reset:', guarded.limits.sqlLength);

for (const db of [primary, replica, clone, guarded]) db.close();
rmSync(dir, { recursive: true, force: true });
