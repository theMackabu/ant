import { test, testThrows, summary } from './helpers.js';

console.log('Temporal Tests\n');

const descriptor = Object.getOwnPropertyDescriptor(globalThis, 'Temporal');
test('Temporal global exists', typeof Temporal, 'object');
test('Temporal global is an own property', descriptor !== undefined, true);
test('Temporal global is writable', descriptor.writable, true);
test('Temporal global is not enumerable', descriptor.enumerable, false);
test('Temporal global is configurable', descriptor.configurable, true);

const constructorNames = ['Duration', 'Instant', 'PlainDate', 'PlainDateTime', 'PlainMonthDay', 'PlainTime', 'PlainYearMonth', 'ZonedDateTime'];

for (const name of constructorNames) {
  test(`Temporal.${name} exists`, typeof Temporal[name], 'function');
  test(`Temporal.${name} prototype brand`, Object.prototype.toString.call(Temporal[name].prototype), `[object Temporal.${name}]`);
}

const date = Temporal.PlainDate.from({ year: 2024, month: 2, day: 31 });
test('PlainDate constrains fields', date.toString(), '2024-02-29');
test('PlainDate arithmetic', date.add({ days: 1 }).toString(), '2024-03-01');
test('PlainDate calendar identifier', date.calendarId, 'iso8601');

const time = Temporal.PlainTime.from('12:34:56.987654321');
test('PlainTime parses nanoseconds', time.nanosecond, 321);
test('PlainTime rounds milliseconds', time.round('millisecond').toString(), '12:34:56.988');

const dateTime = date.toPlainDateTime(time);
test('PlainDateTime date conversion', dateTime.toPlainDate().toString(), '2024-02-29');
test('PlainDateTime time conversion', dateTime.toPlainTime().hour, 12);

const monthDay = Temporal.PlainMonthDay.from('02-29');
test('PlainMonthDay parses month code', monthDay.monthCode, 'M02');
test('PlainMonthDay parses day', monthDay.day, 29);

const yearMonth = Temporal.PlainYearMonth.from('2024-02');
test('PlainYearMonth calendar math', yearMonth.daysInMonth, 29);
test('PlainYearMonth parses year', yearMonth.year, 2024);

const instant = Temporal.Instant.from('2024-03-10T07:30:00Z');
test('Instant epoch nanoseconds round trip', Temporal.Instant.fromEpochNanoseconds(instant.epochNanoseconds).equals(instant), true);
test('Instant epoch milliseconds', instant.epochMilliseconds, 1710055800000);

const zoned = instant.toZonedDateTimeISO('America/New_York');
test('ZonedDateTime uses IANA time-zone data', zoned.toString(), '2024-03-10T03:30:00-04:00[America/New_York]');
test('ZonedDateTime preserves the instant', zoned.toInstant().equals(instant), true);
test('ZonedDateTime exposes its time zone', zoned.timeZoneId, 'America/New_York');

const duration = new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 0, Number.MAX_SAFE_INTEGER, 0);
const roundedDuration = duration.add({ microseconds: Number.MAX_SAFE_INTEGER - 1 });
test('Duration preserves binary64 mathematical values', roundedDuration.microseconds, 18014398509481980);
test('Duration formats large values', roundedDuration.toString(), 'PT18014398509.48198S');
test('Duration balances time units', Temporal.Duration.from({ minutes: 90 }).round('hour').hours, 2);

for (const value of [date, time, dateTime, monthDay, yearMonth, instant, zoned, duration]) {
  testThrows(`${value.constructor.name} relational coercion throws`, () => value < value);
}

test('Temporal.Now.instant returns an Instant', Temporal.Now.instant() instanceof Temporal.Instant, true);
test('Temporal.Now exposes the host time zone', typeof Temporal.Now.timeZoneId(), 'string');

summary();
