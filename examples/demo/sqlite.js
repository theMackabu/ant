import { Database } from 'node:sqlite';

const db = new Database(':memory:');

console.log('Example 1: Schema and inserts');
db.exec(`
  CREATE TABLE books (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    author TEXT NOT NULL,
    year INTEGER,
    rating REAL
  ) STRICT
`);

const insert = db.prepare('INSERT INTO books (title, author, year, rating) VALUES (?, ?, ?, ?)');
const library = [
  ['Dune', 'Frank Herbert', 1965, 4.3],
  ['Neuromancer', 'William Gibson', 1984, 3.9],
  ['Snow Crash', 'Neal Stephenson', 1992, 4.0],
  ['The Dispossessed', 'Ursula K. Le Guin', 1974, 4.2],
  ['Hyperion', 'Dan Simmons', 1989, 4.2]
];

db.exec('BEGIN');
for (const [title, author, year, rating] of library) insert.run(title, author, BigInt(year), rating);
db.exec('COMMIT');
console.log('  inserted', db.prepare('SELECT count(*) AS total FROM books').get().total, 'books');

console.log('\nExample 2: get, all and named parameters');
const byId = db.prepare('SELECT title, author FROM books WHERE id = ?');
console.log('  book 2:', { ...byId.get(2) });
console.log('  book 99:', byId.get(99));

const since = db.prepare('SELECT title, year FROM books WHERE year >= $year AND rating >= $rating ORDER BY year');
for (const book of since.all({ year: 1970, rating: 4 })) console.log(`  ${book.year}  ${book.title}`);

console.log('\nExample 3: Iterating lazily');
const ordered = db.prepare('SELECT title, rating FROM books ORDER BY rating DESC, title');
const top = ordered.iterate().take(3).map(book => `${book.title} (${book.rating})`).toArray();
console.log('  top three:', top.join(', '));

console.log('\nExample 4: Rows as arrays and BigInt reads');
const compact = db.prepare('SELECT id, year FROM books ORDER BY id LIMIT 2', { returnArrays: true, readBigInts: true });
for (const [id, year] of compact.all()) console.log(`  id ${id} (${typeof id}), year ${year}`);

console.log('\nExample 5: Tagged templates');
const sql = db.createTagStore();
const author = 'Neal Stephenson';
console.log('  found:', sql.get`SELECT title FROM books WHERE author = ${author}`.title);
console.log('  updated:', sql.run`UPDATE books SET rating = ${4.5} WHERE title = ${'Dune'}`.changes, 'row');
console.log('  cached statements:', sql.size);

console.log('\nExample 6: Transactions');
const rename = db.prepare('UPDATE books SET title = ? WHERE id = ?');
try {
  db.exec('BEGIN');
  rename.run('Dune Messiah', 1);
  console.log('  in transaction:', db.isTransaction);
  rename.run(null, 2);
  db.exec('COMMIT');
} catch (error) {
  db.exec('ROLLBACK');
  console.log('  rolled back:', error.message);
}
console.log('  book 1 is still:', byId.get(1).title);

console.log('\nExample 7: Statement metadata');
const report = db.prepare('SELECT title AS name, rating * 2 AS score FROM books WHERE year > ?');
report.all(1980);
console.log('  columns:', report.columns().map(column => `${column.name} <- ${column.table ?? 'expression'}`));
console.log('  expanded:', report.expandedSQL);
console.log('  rows scanned:', report.stat('fullscanStep'));

db.close();
console.log('\nclosed:', !db.isOpen);
