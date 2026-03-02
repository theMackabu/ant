import { printTable } from '../formatter.js';

import chalk from 'chalk';
import semver from 'semver';
import dotenv from 'dotenv';

import { marked } from 'marked';
import { minimatch } from 'minimatch';
import { stringify as stringifyYaml } from 'yaml';
import { parse as parseCsv } from 'csv-parse/sync';

const yaml = stringifyYaml({
  name: 'test',
  version: '1.0.0',
  tags: ['runtime', 'modules']
}).trim();

const env = dotenv.parse('HOST=localhost\nPORT=3000');
const csv = parseCsv('name,age\nalice,30\nbob,25', { columns: true });

const rows = [
  ['Small dependency trees', 'output'],
  ['semver.valid', semver.valid('1.2.3')],
  ['semver.gt', semver.gt('2.0.0', '1.0.0')],
  ['minimatch', minimatch('/foo/bar.js', '/foo/**/*.js')],
  ['dotenv', `HOST=${env.HOST} PORT=${env.PORT}`],
  ['csv-parse', `${csv[0].name}(${csv[0].age}), ${csv[1].name}(${csv[1].age})`],
  ['marked', marked('**bold** and _italic_').trim()]
];

printTable(rows);

console.log(yaml);
console.log(chalk.green('green') + ' ' + chalk.bold.red('bold red'));
