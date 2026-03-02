import { printTable } from '../formatter.js';

import Ajv from 'ajv';
import Handlebars from 'handlebars';
import { format, addDays, differenceInDays, formatDistance } from 'date-fns';

const now = new Date('2026-02-27T12:00:00Z');
const later = addDays(now, 10);
const formatted = format(now, 'yyyy-MM-dd');
const distance = formatDistance(now, later);
const daysDiff = differenceInDays(later, now);

const ajv = new Ajv();
const schema = {
  type: 'object',
  properties: {
    name: { type: 'string' },
    age: { type: 'integer', minimum: 0 }
  },
  required: ['name', 'age']
};

const validate = ajv.compile(schema);
const valid = validate({ name: 'alice', age: 30 });
const invalid = validate({ name: 'bob', age: -1 });

const template = Handlebars.compile('Hello {{name}}, you have {{count}} messages');
const rendered = template({ name: 'Alice', count: 5 });

const rows = [
  ['Real world tools', 'output'],
  ['date-fns format', formatted],
  ['date-fns addDays', format(later, 'yyyy-MM-dd')],
  ['date-fns distance', distance],
  ['date-fns diffDays', daysDiff],
  ['ajv valid', valid],
  ['ajv invalid', `${invalid} — ${validate.errors?.[0]?.message}`],
  ['handlebars', rendered]
];

printTable(rows);
