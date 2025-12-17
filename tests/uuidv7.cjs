// Test UUIDv7 generation from crypto.randomUUIDv7()

function validateUUIDv7Format(uuid) {
  // Check format: xxxxxxxx-xxxx-7xxx-xxxx-xxxxxxxxxxxx
  // Must be exactly 36 characters
  if (uuid.length !== 36) return false;

  // Check dash positions
  if (uuid[8] !== '-' || uuid[13] !== '-' || uuid[18] !== '-' || uuid[23] !== '-') {
    return false;
  }

  // Check version digit (should be 7)
  if (uuid[14] !== '7') return false;

  // Check variant digit (should be 8, 9, a, or b)
  const variantChar = uuid[19];
  if (variantChar !== '8' && variantChar !== '9' && variantChar !== 'a' && variantChar !== 'b') {
    return false;
  }

  // Check all other characters are hex digits
  for (let i = 0; i < uuid.length; i++) {
    if (i === 8 || i === 13 || i === 18 || i === 23) continue; // Skip dashes
    const c = uuid[i];
    const isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!isHex) return false;
  }

  return true;
}

function extractTimestamp(uuid) {
  // UUIDv7 format: timestamp is in the first 48 bits (first 12 hex chars without dashes)
  // Remove all dashes using multiple replace calls (replace only replaces first occurrence)
  let hex = uuid;
  hex = hex.replace('-', '');
  hex = hex.replace('-', '');
  hex = hex.replace('-', '');
  hex = hex.replace('-', '');

  const timestampHex = hex.substring(0, 12);

  // Convert hex to number (timestamp in milliseconds)
  let timestamp = 0;
  for (let i = 0; i < timestampHex.length; i++) {
    const digit = timestampHex[i];
    const value = digit >= '0' && digit <= '9' ? digit.charCodeAt(0) - '0'.charCodeAt(0) : digit.charCodeAt(0) - 'a'.charCodeAt(0) + 10;
    timestamp = timestamp * 16 + value;
  }

  return timestamp;
}

// Test 1: Generate and validate format
console.log('Test 1: Basic UUIDv7 generation');
const uuid1 = crypto.randomUUIDv7();
console.log('Generated:', uuid1);
console.log('Valid format:', validateUUIDv7Format(uuid1) ? 'PASS' : 'FAIL');
console.log('Length:', uuid1.length === 36 ? 'PASS' : 'FAIL');
console.log('');

// Test 2: Uniqueness - generate multiple UUIDs
console.log('Test 2: Uniqueness check');
const uuids = [];
const count = 100;
for (let i = 0; i < count; i++) {
  uuids[i] = crypto.randomUUIDv7();
}

let allUnique = true;
for (let i = 0; i < count; i++) {
  for (let j = i + 1; j < count; j++) {
    if (uuids[i] === uuids[j]) {
      allUnique = false;
      console.log('Collision found:', uuids[i]);
      break;
    }
  }
  if (!allUnique) break;
}
console.log('Generated', count, 'unique UUIDs:', allUnique ? 'PASS' : 'FAIL');
console.log('');

// Test 3: Timestamp ordering
console.log('Test 3: Timestamp ordering');
const uuid2 = crypto.randomUUIDv7();
const uuid3 = crypto.randomUUIDv7();

const ts2 = extractTimestamp(uuid2);
const ts3 = extractTimestamp(uuid3);

console.log('UUID 1:', uuid2);
console.log('Timestamp 1:', ts2);
console.log('UUID 2:', uuid3);
console.log('Timestamp 2:', ts3);
console.log('Chronologically ordered:', ts3 >= ts2 ? 'PASS' : 'FAIL');
console.log('');

// Test 4: Version and variant bits
console.log('Test 4: Version and variant validation');
const testUuid = crypto.randomUUIDv7();
const parts = testUuid.split('-');

// Check version (4 bits at position 48-51, should be 0111 = 7)
const versionChar = parts[2][0];
console.log('Version character:', versionChar);
console.log('Version is 7:', versionChar === '7' ? 'PASS' : 'FAIL');

// Check variant (2 bits at position 64-65, should be 10)
const variantChar = parts[3][0];
const variantValid = variantChar === '8' || variantChar === '9' || variantChar === 'a' || variantChar === 'b';
console.log('Variant character:', variantChar);
console.log('Variant is RFC 4122:', variantValid ? 'PASS' : 'FAIL');
console.log('');

// Test 5: Timestamp reasonableness
console.log('Test 5: Timestamp reasonableness');
const currentUuid = crypto.randomUUIDv7();
const currentTs = extractTimestamp(currentUuid);

// Timestamp should be reasonable (after 2020-01-01 and before 2100-01-01)
const ts2020 = 1577836800000; // 2020-01-01 in ms
const ts2100 = 4102444800000; // 2100-01-01 in ms

console.log('Extracted timestamp:', currentTs, 'ms');
console.log('After 2020:', currentTs > ts2020 ? 'PASS' : 'FAIL');
console.log('Before 2100:', currentTs < ts2100 ? 'PASS' : 'FAIL');
console.log('');

// Test 6: Display multiple UUIDs
console.log('Test 6: Sample UUIDv7s');
for (let i = 0; i < 5; i++) {
  console.log(i + 1 + ':', crypto.randomUUIDv7());
}
