const myArray = ['a', 1, 'a', 2, '1'];
const unique = Array.from(new Set(myArray));
console.log('Set:', unique);

const doubled = Array.from(new Set([1, 2, 3]), x => x * 2);
console.log('Set+map:', doubled);

console.log('Array:', Array.from([10, 20, 30]));
console.log('String:', Array.from("abc"));
