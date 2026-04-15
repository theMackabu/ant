let resizeCount = 0;

process.stdout.on('resize', () => {
  resizeCount++;
  console.log(`RESIZE ${process.stdout.rows} ${process.stdout.columns}`);
  setTimeout(() => process.exit(0), 25);
});

console.log('READY');

setTimeout(() => {
  if (resizeCount === 0) {
    console.error('resize event did not fire');
    process.exit(2);
  }
}, 3000);
