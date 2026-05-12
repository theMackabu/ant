function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const cyrillic = String.fromCharCode(0x0410, 0x0411, 0x0412);
assert(cyrillic === 'АБВ', `expected Cyrillic string, got ${JSON.stringify(cyrillic)}`);
assert(cyrillic.charCodeAt(0) === 0x0410, 'first code unit should be U+0410');
assert(String.fromCharCode(0x1f600) === '\uf600', 'fromCharCode should truncate to 16 bits');

console.log('String.fromCharCode unicode ok');
