// Reproduce JIT crash: Array.map callback with string concatenation + upvalues
// The callback uses OP_ADD on strings (triggers bailout) and OP_GET_UPVAL.

const codes = { reset: '\x1b[0m' };

function createGrid(width, height) {
  const chars = Array(height).fill(null).map(() => Array(width).fill(' '));
  const styles = Array(height).fill(null).map(() => Array(width).fill(''));
  return { chars, styles };
}

function render() {
  const width = 20;
  const height = 10;
  const grid = createGrid(width, height);

  // Set some styled cells
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      if (x === 5) grid.styles[y][x] = '\x1b[32m';
    }
  }

  // This .map() callback captures `grid` and `codes` as upvalues,
  // and does string concatenation (OP_ADD with strings → JIT bailout).
  const rows = grid.chars.map((row, y) => {
    let line = '';
    let currentStyle = '';
    for (let x = 0; x < row.length; x++) {
      const nextStyle = grid.styles[y][x];
      if (nextStyle !== currentStyle) {
        line += nextStyle || codes.reset;
        currentStyle = nextStyle;
      }
      line += row[x];
    }
    return line + codes.reset;
  });

  return rows.join('\n');
}

// Call render() enough times to trigger JIT compilation of the .map() callback.
// With SV_JIT_THRESHOLD=100 and 10 rows per render, ~11 renders should trigger it.
let result;
for (let i = 0; i < 20; i++) {
  result = render();
}

console.log('OK: JIT map+string concat survived', result.length, 'chars');
