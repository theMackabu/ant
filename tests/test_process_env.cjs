console.log('PATH:', process.env.PATH);
console.log('HOME:', process.env.HOME);
console.log('USER:', process.env.USER);

console.log('CAT:', process.env.CAT);

if (process.env.CAT === 'meow') {
  console.log("✓ process.env.CAT is correctly set to 'meow'");
} else {
  console.log('✗ process.env.CAT is not set correctly. Got:', process.env.CAT);
}

console.log('ANT_ENV', process.env.ANT_ENV);
process.exit(256);
