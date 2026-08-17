const raw = Number(process.env.MARCH_COUNT ?? 5);
const upTo = Number.isInteger(raw) && raw >= 1 && raw <= 1000 ? raw : 5;

for (let i = 1; i <= upTo; i++) {
  console.log(`ant ${i} reporting in`);
}
