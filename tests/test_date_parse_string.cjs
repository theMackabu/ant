let failed = 0;

function check(label, cond, detail) {
  if (!cond) {
    failed++;
    console.log("FAIL", label, detail || "");
  }
}

const msZ = Date.parse("2026-02-27T12:00:00Z");
check("Date.parse Z", msZ === 1772193600000, msZ);

const dZ = new Date("2026-02-27T12:00:00Z");
check("new Date Z toISOString", dZ.toISOString() === "2026-02-27T12:00:00.000Z", dZ.toISOString());

const dDateOnly = new Date("2026-02-27");
check("date-only is UTC midnight", dDateOnly.toISOString() === "2026-02-27T00:00:00.000Z", dDateOnly.toISOString());

const dClone = new Date(dZ);
check("clone from Date object", dClone.getTime() === dZ.getTime(), dClone.getTime() + " vs " + dZ.getTime());

const msOff = Date.parse("2026-02-27T12:00:00+02:30");
const msOffRef = Date.parse("2026-02-27T09:30:00Z");
check("timezone offset", msOff === msOffRef, msOff + " vs " + msOffRef);

const invalidParse = Date.parse("not-a-date");
check("invalid parse is NaN", invalidParse !== invalidParse, invalidParse);

const invalidDate = new Date("not-a-date");
const invalidTime = invalidDate.getTime();
check("invalid date getTime is NaN", invalidTime !== invalidTime, invalidTime);

if (failed > 0) throw new Error("test_date_parse_string failed");
