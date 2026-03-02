import { printTable } from '../formatter.js';

import { z } from 'zod';
import { diff } from 'radash';
import { nanoid } from 'nanoid';
import { marked } from 'marked';
import { merge } from 'lodash-es';
import { v4 as uuidv4 } from 'uuid';

import ms from 'ms';
import slugify from 'slugify';
import prettyBytes from 'pretty-bytes';

const oldWorldGods = ['ra', 'zeus'];
const newWorldGods = ['vishnu', 'zeus'];

const rows = [
  ['Zero dependencies', 'output'],
  ['uuid', uuidv4()],
  ['zod', z.string().parse('it works')],
  ["ms('2 days')", ms('2 days')],
  ['prettyBytes', prettyBytes(1337)],
  ['slugify', slugify('Hello World!')],
  ['deepmerge', merge({ a: 1 }, { b: 2 })],
  ['diff', diff(oldWorldGods, newWorldGods)],
  ['nanoid', nanoid()],
  ['marked', marked('# Hello').trim()]
];

printTable(rows);
