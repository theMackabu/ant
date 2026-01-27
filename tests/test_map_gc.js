console.log('=== Map GC Debug ===\n');

let map = new Map();
console.log('Created map');

map.set('key1', { data: 'value1' });
console.log('Set key1, size:', map.size);

map.set('key2', [1, 2, 3]);
console.log('Set key2, size:', map.size);

map.set('key3', 'simple string');
console.log('Set key3, size:', map.size);

console.log('\nBefore GC:');
console.log('  map.size:', map.size);
console.log('  map.get("key1"):', map.get('key1'));
console.log('  map.get("key2"):', map.get('key2'));
console.log('  map.get("key3"):', map.get('key3'));

Ant.gc();
console.log('\nGC requested');

console.log('\nAfter GC:');
console.log('  map.size:', map.size);
console.log('  map.get("key1"):', map.get('key1'));
console.log('  map.get("key2"):', map.get('key2'));
console.log('  map.get("key3"):', map.get('key3'));

console.log('\n=== Done ===');
