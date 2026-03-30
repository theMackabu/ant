const samples = [
  "$n0",
  "$n1",
  "$n123",
  "$n-42",
  "$n0x10",
];

for (const value of samples) {
  try {
    const parsed = BigInt(value.slice(2));
    console.log(`${value} -> ${parsed.toString()}`);
  } catch (error) {
    console.log(`${value} -> error:${error && error.message ? error.message : String(error)}`);
  }
}
