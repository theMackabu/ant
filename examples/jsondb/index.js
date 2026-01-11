import JSONdb from './jsondb.js';
import { unlinkSync } from 'fs';

const db = new JSONdb('./mydata.json');

db.set('username', 'alice');
db.set('score', 42);
db.set('settings', {
  theme: 'dark',
  notifications: true,
  language: 'en'
});
db.set('tags', ['javascript', 'nodejs', 'database']);

console.log('Username:', db.get('username'));
console.log('Score:', db.get('score'));
console.log('Settings:', db.get('settings'));

console.log('Has username?', db.has('username'));
console.log('Has email?', db.has('email'));

console.log('Email:', db.get('email'));

db.set('score', 100);
console.log('New score:', db.get('score'));

db.delete('tags');
console.log('Has tags?', db.has('tags'));

const snapshot = db.JSON();
console.log('Full DB:', snapshot);

db.JSON({ fresh: 'start', count: 1 });
console.log('After replace:', db.JSON());

db.deleteAll();
console.log('After deleteAll:', db.JSON()); // {}

const customDb = new JSONdb('./custom.json', {
  asyncWrite: false,
  syncOnWrite: false,
  jsonSpaces: 2
});

customDb.set('key1', 'value1');
customDb.set('key2', 'value2');
customDb.sync();

try {
  unlinkSync('./mydata.json');
  unlinkSync('./custom.json');
  console.log('\nTest files cleaned up successfully.');
} catch (err) {
  console.error('Cleanup error:', err.message);
}
