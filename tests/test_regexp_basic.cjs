// Test basic RegExp constructor
const re1 = new RegExp('hello');
Ant.println('RegExp created:', re1.source);
Ant.println('Flags:', re1.flags);
Ant.println('Global:', re1.global);

// Test with flags
const re2 = new RegExp('test', 'g');
Ant.println('Global flag set:', re2.global);
Ant.println('Flags:', re2.flags);

// Test multiple flags
const re3 = new RegExp('pattern', 'gi');
Ant.println('Multiple flags:', re3.flags);
Ant.println('Global:', re3.global);
Ant.println('IgnoreCase:', re3.ignoreCase);

Ant.println('All basic RegExp tests passed!');
