import { readFile, writeFile, unlink, mkdir, rmdir, stat } from 'ant:fs';

console.log('Testing ant:fs with async/await...\n');

async function testFs() {
  const testDir = 'tests/.fs_test_tmp';
  const testFile = testDir + '/async_test.txt';
  const testData = 'Hello from async ant:fs!';

  try {
    // Test mkdir
    console.log('=== Test: mkdir ===');
    try {
      await mkdir(testDir);
      console.log('✓ Directory created');
    } catch (e) {
      console.log('Directory might exist, continuing...');
    }

    // Test writeFile
    console.log('\n=== Test: writeFile ===');
    await writeFile(testFile, testData);
    console.log('✓ File written');

    // Test readFile
    console.log('\n=== Test: readFile ===');
    const content = await readFile(testFile);
    console.log('✓ File read, length:', content.length);
    if (content === testData) {
      console.log('✓ Content matches!');
    } else {
      console.log('✗ Content mismatch!');
    }

    // Test stat
    console.log('\n=== Test: stat ===');
    const stats = await stat(testFile);
    console.log('✓ File stats:');
    console.log('  Size:', stats.size);
    console.log('  Is file:', stats.isFile);
    console.log('  Is directory:', stats.isDirectory);

    // Cleanup
    console.log('\n=== Cleanup ===');
    await unlink(testFile);
    console.log('✓ File deleted');
    
    await rmdir(testDir);
    console.log('✓ Directory deleted');

    console.log('\n✓✓✓ All tests passed! ✓✓✓');
  } catch (error) {
    console.error('\n✗ Test failed:', error);
  }
}

// Run the async function
testFs();
