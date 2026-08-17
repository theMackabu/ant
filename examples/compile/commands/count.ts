const upTo = Number(process.env.MARCH_COUNT ?? 5);

for (let i = 1; i <= upTo; i++) {
  console.log(`ant ${i} reporting in`);
}

process.exit(0);
