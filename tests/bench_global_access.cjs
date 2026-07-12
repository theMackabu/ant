globalThis.globalAccessValue = 7;

let sum = 0;
for (let i = 0; i < 20_000_000; i++) sum += globalAccessValue;
if (sum !== 140_000_000) throw new Error('incorrect global read result');

for (let i = 0; i < 10_000_000; i++) globalAccessValue = i;
if (globalAccessValue !== 9_999_999) throw new Error('incorrect global write result');

console.log('global access benchmark passed');
