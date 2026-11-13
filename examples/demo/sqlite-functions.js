import { Database, constants } from 'node:sqlite';

const db = new Database(':memory:');
db.exec(`
  CREATE TABLE orders (id INTEGER PRIMARY KEY, customer TEXT, item TEXT, cents INTEGER);
  INSERT INTO orders (customer, item, cents) VALUES
    ('ada', 'keyboard', 8900), ('ada', 'cable', 1200), ('ada', 'monitor', 32900),
    ('lin', 'mouse', 4500), ('lin', 'desk', 41000), ('sam', 'lamp', 2600);
`);

console.log('Example 1: Scalar functions');
db.function('dollars', cents => `$${(cents / 100).toFixed(2)}`);
db.function('initials', { varargs: true }, (...words) => words.map(word => word[0].toUpperCase()).join(''));

for (const row of db.prepare('SELECT item, dollars(cents) AS price, initials(customer, item) AS tag FROM orders LIMIT 3').all()) {
  console.log(`  ${row.tag}  ${row.item.padEnd(10)} ${row.price}`);
}

console.log('\nExample 2: Aggregates');
db.aggregate('median', {
  start: () => [],
  step: (values, value) => (values.push(value), values),
  result: values => values.sort((a, b) => a - b)[Math.floor(values.length / 2)]
});

for (const row of db.prepare('SELECT customer, median(cents) AS middle, count(*) AS orders FROM orders GROUP BY customer').all()) {
  console.log(`  ${row.customer}: median ${row.middle} cents across ${row.orders} orders`);
}

console.log('\nExample 3: Window functions');
db.aggregate('running', {
  start: 0,
  step: (total, cents) => total + cents,
  inverse: (total, cents) => total - cents
});

const running = db.prepare(`
  SELECT customer, item, running(cents) OVER (PARTITION BY customer ORDER BY id) AS spent
  FROM orders ORDER BY customer, id
`);
for (const row of running.all()) console.log(`  ${row.customer}  ${row.item.padEnd(10)} ${row.spent}`);

console.log('\nExample 4: Virtual tables backed by JavaScript');
db.createModule('fibonacci', {
  columns: [
    { name: 'n', type: 'INTEGER' },
    { name: 'value', type: 'INTEGER' },
    { name: 'count', type: 'INTEGER', hidden: true }
  ],
  *rows(count = 10) {
    let [a, b] = [0n, 1n];
    for (let n = 0n; n < BigInt(count); n++) {
      yield [n, a];
      [a, b] = [b, a + b];
    }
  }
});

console.log('  first twelve:', db.prepare("SELECT group_concat(value, ' ') AS sequence FROM fibonacci(12)").get().sequence);
console.log('  even terms:', db.prepare('SELECT count(*) AS total FROM fibonacci(40) WHERE value % 2 = 0').get().total);

console.log('\nExample 5: Authorizer');
db.setAuthorizer((action, table) => {
  if (action === constants.SQLITE_DELETE && table === 'orders') return constants.SQLITE_DENY;
  return constants.SQLITE_OK;
});

try {
  db.exec('DELETE FROM orders');
} catch (error) {
  console.log('  blocked:', error.message);
}

db.setAuthorizer(null);
console.log('  orders remaining:', db.prepare('SELECT count(*) AS total FROM orders').get().total);

console.log('\nExample 6: Errors thrown in callbacks propagate');
db.function('explode', () => {
  throw new RangeError('not today');
});

try {
  db.prepare('SELECT explode()').get();
} catch (error) {
  console.log(`  ${error.name}: ${error.message}`);
}

db.close();
