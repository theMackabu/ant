function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function assertSame(actual, expected, message) {
  if (!Object.is(actual, expected)) {
    throw new Error(`${message}: expected ${String(expected)}, got ${String(actual)}`);
  }
}

function assertThrows(errorType, fn, message) {
  try {
    fn();
  } catch (error) {
    if (error instanceof errorType) return;
    throw new Error(`${message}: expected ${errorType.name}, got ${error?.constructor?.name}`);
  }
  throw new Error(`${message}: expected ${errorType.name}`);
}

const descriptor = Object.getOwnPropertyDescriptor(globalThis, "Temporal");
assertSame(typeof Temporal, "object", "Temporal is installed globally");
assert(descriptor !== undefined, "Temporal has a global descriptor");
assertSame(descriptor.writable, true, "Temporal is writable");
assertSame(descriptor.enumerable, false, "Temporal is not enumerable");
assertSame(descriptor.configurable, true, "Temporal is configurable");

for (const name of [
  "Duration", "Instant", "PlainDate", "PlainDateTime", "PlainMonthDay",
  "PlainTime", "PlainYearMonth", "ZonedDateTime",
]) {
  assertSame(typeof Temporal[name], "function", `Temporal.${name} exists`);
  assertSame(Object.prototype.toString.call(Temporal[name].prototype),
    `[object Temporal.${name}]`, `Temporal.${name} has the right brand`);
}

const date = Temporal.PlainDate.from({ year: 2024, month: 2, day: 31 });
assertSame(date.toString(), "2024-02-29", "PlainDate constrains fields");
assertSame(date.add({ days: 1 }).toString(), "2024-03-01", "PlainDate arithmetic");

const time = Temporal.PlainTime.from("12:34:56.987654321");
assertSame(time.round("millisecond").toString(), "12:34:56.988", "PlainTime rounding");

const dateTime = date.toPlainDateTime(time);
assertSame(dateTime.toPlainDate().toString(), "2024-02-29", "PlainDateTime date conversion");
assertSame(dateTime.toPlainTime().hour, 12, "PlainDateTime time conversion");

assertSame(Temporal.PlainMonthDay.from("02-29").monthCode, "M02", "PlainMonthDay parsing");
assertSame(Temporal.PlainYearMonth.from("2024-02").daysInMonth, 29, "PlainYearMonth calendar math");

const instant = Temporal.Instant.from("2024-03-10T07:30:00Z");
assertSame(Temporal.Instant.fromEpochNanoseconds(instant.epochNanoseconds).equals(instant),
  true, "Instant epoch conversion");

const zoned = instant.toZonedDateTimeISO("America/New_York");
assertSame(zoned.toString(), "2024-03-10T03:30:00-04:00[America/New_York]",
  "ZonedDateTime uses the host IANA database");
assertSame(zoned.toInstant().equals(instant), true, "ZonedDateTime preserves the instant");

const duration = new Temporal.Duration(
  0, 0, 0, 0, 0, 0, 0, 0, Number.MAX_SAFE_INTEGER, 0);
const roundedDuration = duration.add({ microseconds: Number.MAX_SAFE_INTEGER - 1 });
assertSame(roundedDuration.microseconds, 18014398509481980,
  "Duration fields use binary64 mathematical values");
assertSame(roundedDuration.toString(), "PT18014398509.48198S",
  "Duration formatting cannot observe extra native precision");

for (const value of [date, time, dateTime, instant, zoned, duration]) {
  assertThrows(TypeError, () => value < value, "Temporal relational coercion throws");
}

assert(Temporal.Now.instant() instanceof Temporal.Instant, "Temporal.Now.instant returns an Instant");
assertSame(typeof Temporal.Now.timeZoneId(), "string", "Temporal.Now exposes the host time zone");

// Exercise native finalizers and provider-backed values through several heap cycles.
for (let i = 0; i < 2000; i++) {
  Temporal.ZonedDateTime.from(`2024-01-${String(i % 28 + 1).padStart(2, "0")}T12:00Z[UTC]`)
    .add({ hours: i % 24 })
    .toPlainDateTime();
}

console.log("Temporal API tests passed");
