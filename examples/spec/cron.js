import { test, testThrows, summary } from './helpers.js';

console.log('Cron Tests\n');

test('Ant.cron exists', typeof Ant.cron, 'function');
test('Ant.cron arity', Ant.cron.length, 3);
test('Ant.cron.parse exists', typeof Ant.cron.parse, 'function');
test('Ant.cron.remove exists', typeof Ant.cron.remove, 'function');

const origin = new Date('2026-01-01T00:00:01Z');
const utc = { tz: 'UTC' };
const next = (expression, relative = origin) => Ant.cron.parse(expression, relative, utc);

test('parse returns Date', next('@hourly') instanceof Date, true);
test('hourly nickname', next('@hourly').toISOString(), '2026-01-01T01:00:00.000Z');
test('minute step', next('*/15 * * * *').toISOString(), '2026-01-01T00:15:00.000Z');
test('named weekday range', next('0 9 * * MON-FRI').toISOString(), '2026-01-01T09:00:00.000Z');
test('POSIX day OR matching', next('0 0 15 * FRI').toISOString(), '2026-01-02T00:00:00.000Z');
test('impossible date returns null', next('0 0 30 2 *'), null);
testThrows('invalid expression throws', () => Ant.cron.parse('* * * *', origin, utc));
testThrows('invalid time zone throws', () => Ant.cron.parse('@hourly', origin, { tz: 'Not/A_Zone' }));

const job = Ant.cron('@hourly', () => {}, utc);
test('CronJob brand', Object.prototype.toString.call(job), '[object CronJob]');
test('CronJob cron getter', job.cron, '@hourly');
test('CronJob unref is chainable', job.unref(), job);
test('CronJob ref is chainable', job.ref(), job);
test('CronJob stop is chainable', job.stop(), job);
test('CronJob dispose returns undefined', job[Symbol.dispose](), undefined);

summary();
