// Test timer list mutation during callback execution
// This tests the case where a setTimeout callback adds new timers

let results = [];
let expected = ['timer1', 'timer2', 'timer3', 'done'];

// Timer 1 fires and adds timer 2 at head of list
setTimeout(() => {
  results.push('timer1');
  
  // Add a new timer with 0ms delay - inserts at head of timer list
  setTimeout(() => {
    results.push('timer2');
    
    // Add another timer from within timer2
    setTimeout(() => {
      results.push('timer3');
    }, 0);
  }, 0);
}, 10);

// Final check after all timers should have fired
setTimeout(() => {
  results.push('done');
  
  const passed = JSON.stringify(results) === JSON.stringify(expected);
  console.log('Results:', JSON.stringify(results));
  console.log('Expected:', JSON.stringify(expected));
  console.log('Test:', passed ? 'PASSED' : 'FAILED');
  
  if (!passed) {
    process.exit(1);
  }
}, 100);

console.log('Timer mutation test started...');
