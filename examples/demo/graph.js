const data = {
  0: 2,
  1: 5,
  2: 8,
  3: 12,
  4: 15,
  5: 18,
  6: 16,
  7: 13,
  8: 10,
  9: 7,
  10: 4
};

const config = {
  width: 60,
  height: 20,
  xLabel: 'Time',
  yLabel: 'Value',
  barChar: '█',
  pointChar: '●',
  axisChar: '│',
  gridChar: '·'
};

function getMinMax(dataObj) {
  const keys = Object.keys(dataObj);
  if (keys.length === 0) {
    return { min: 0, max: 0, xMin: 0, xMax: 0 };
  }

  let yMin = dataObj[keys[0]];
  let yMax = dataObj[keys[0]];
  let xMin = parseInt(keys[0]);
  let xMax = parseInt(keys[0]);

  for (let i = 0; i < keys.length; i = i + 1) {
    const key = keys[i];
    const value = dataObj[key];
    const xValue = parseInt(key);

    if (value < yMin) yMin = value;
    if (value > yMax) yMax = value;
    if (xValue < xMin) xMin = xValue;
    if (xValue > xMax) xMax = xValue;
  }

  return { min: yMin, max: yMax, xMin: xMin, xMax: xMax };
}

function scale(value, dataMin, dataMax, displayMin, displayMax) {
  if (dataMax === dataMin) return displayMin;
  const ratio = (value - dataMin) / (dataMax - dataMin);
  return displayMin + ratio * (displayMax - displayMin);
}

function drawLineGraph(dataObj) {
  const stats = getMinMax(dataObj);
  const keys = Object.keys(dataObj);

  console.log('\nLine graph:\n');
  console.log('Data Range: ' + stats.min + ' to ' + stats.max);
  console.log('X Range: ' + stats.xMin + ' to ' + stats.xMax);
  console.log('');

  const grid = [];
  for (let y = 0; y < config.height; y = y + 1) {
    const row = [];
    for (let x = 0; x < config.width; x = x + 1) row.push(' ');
    grid.push(row);
  }

  for (let i = 0; i < keys.length; i = i + 1) {
    const key = keys[i];
    const xValue = parseInt(key);
    const yValue = dataObj[key];

    const x = Math.floor(scale(xValue, stats.xMin, stats.xMax, 0, config.width - 1));
    const y = Math.floor(scale(yValue, stats.min, stats.max, config.height - 1, 0));

    if (x >= 0 && x < config.width && y >= 0 && y < config.height) {
      grid[y][x] = config.pointChar;
    }

    if (i < keys.length - 1) {
      const nextKey = keys[i + 1];
      const nextX = parseInt(nextKey);
      const nextY = dataObj[nextKey];

      const x1 = Math.floor(scale(xValue, stats.xMin, stats.xMax, 0, config.width - 1));
      const y1 = Math.floor(scale(yValue, stats.min, stats.max, config.height - 1, 0));
      const x2 = Math.floor(scale(nextX, stats.xMin, stats.xMax, 0, config.width - 1));
      const y2 = Math.floor(scale(nextY, stats.min, stats.max, config.height - 1, 0));

      const dx = x2 - x1;
      const dy = y2 - y1;
      const steps = Math.max(Math.abs(dx), Math.abs(dy));

      for (let step = 0; step <= steps; step = step + 1) {
        const t = steps === 0 ? 0 : step / steps;
        const ix = Math.floor(x1 + t * dx);
        const iy = Math.floor(y1 + t * dy);

        if (ix >= 0 && ix < config.width && iy >= 0 && iy < config.height) {
          if (grid[iy][ix] === ' ') {
            grid[iy][ix] = config.gridChar;
          }
        }
      }
    }
  }

  const yLabelWidth = ('' + stats.max).length;

  for (let y = 0; y < config.height; y = y + 1) {
    const yValue = scale(y, 0, config.height - 1, stats.max, stats.min);
    const yLabel = '' + Math.floor(yValue);
    const paddedLabel = yLabel.padStart(yLabelWidth, ' ');

    let line = paddedLabel + ' ' + config.axisChar;
    for (let x = 0; x < config.width; x = x + 1) {
      line = line + grid[y][x];
    }
    console.log(line);
  }

  let xAxis = '';
  for (let i = 0; i < yLabelWidth + 1; i = i + 1) {
    xAxis = xAxis + ' ';
  }
  xAxis = xAxis + '└';
  for (let x = 0; x < config.width; x = x + 1) {
    xAxis = xAxis + '─';
  }
  console.log(xAxis);

  let xLabels = '';
  for (let i = 0; i < yLabelWidth + 2; i = i + 1) {
    xLabels = xLabels + ' ';
  }
  xLabels = xLabels + ('' + stats.xMin);
  const endLabel = '' + stats.xMax;
  const middleSpace = config.width - ('' + stats.xMin).length - endLabel.length;
  for (let i = 0; i < middleSpace; i = i + 1) {
    xLabels = xLabels + ' ';
  }
  xLabels = xLabels + endLabel;
  console.log(xLabels);
  console.log('');
}

function drawBarChart(dataObj) {
  const stats = getMinMax(dataObj);
  const keys = Object.keys(dataObj);

  console.log('\nBar Chart:\n');
  console.log('Data Range: ' + stats.min + ' to ' + stats.max);
  console.log('');

  const yLabelWidth = ('' + stats.max).length;
  const barWidth = Math.floor((config.width - keys.length + 1) / keys.length);

  const grid = [];
  for (let y = 0; y < config.height; y = y + 1) {
    const row = [];
    for (let x = 0; x < config.width; x = x + 1) {
      row.push(' ');
    }
    grid.push(row);
  }

  for (let i = 0; i < keys.length; i = i + 1) {
    const key = keys[i];
    const value = dataObj[key];
    const barHeight = Math.floor(scale(value, stats.min, stats.max, 0, config.height));
    const xPos = i * (barWidth + 1);

    for (let h = 0; h < barHeight; h = h + 1) {
      const y = config.height - 1 - h;
      for (let w = 0; w < barWidth && xPos + w < config.width; w = w + 1) {
        grid[y][xPos + w] = config.barChar;
      }
    }
  }

  for (let y = 0; y < config.height; y = y + 1) {
    const yValue = scale(y, 0, config.height - 1, stats.max, stats.min);
    const yLabel = '' + Math.floor(yValue);
    const paddedLabel = yLabel.padStart(yLabelWidth, ' ');

    let line = paddedLabel + ' ' + config.axisChar;
    for (let x = 0; x < config.width; x = x + 1) {
      line = line + grid[y][x];
    }
    console.log(line);
  }

  let xAxis = '';
  for (let i = 0; i < yLabelWidth + 1; i = i + 1) {
    xAxis = xAxis + ' ';
  }
  xAxis = xAxis + '└';
  for (let x = 0; x < config.width; x = x + 1) {
    xAxis = xAxis + '─';
  }
  console.log(xAxis);

  let xLabels = '';
  for (let i = 0; i < yLabelWidth + 2; i = i + 1) {
    xLabels = xLabels + ' ';
  }
  for (let i = 0; i < keys.length; i = i + 1) {
    const key = keys[i];
    xLabels = xLabels + key;
    for (let j = 0; j < barWidth; j = j + 1) {
      xLabels = xLabels + ' ';
    }
  }
  console.log(xLabels);
  console.log('');
}

function drawSparkline(dataObj) {
  const keys = Object.keys(dataObj);
  const stats = getMinMax(dataObj);

  console.log('\nSparkline:\n');

  const blocks = ['▁', '▂', '▃', '▄', '▅', '▆', '▇', '█'];
  let sparkline = '';

  for (let i = 0; i < keys.length; i = i + 1) {
    const key = keys[i];
    const value = dataObj[key];
    const normalized = scale(value, stats.min, stats.max, 0, blocks.length - 1);
    const blockIndex = Math.floor(normalized);
    sparkline = sparkline + blocks[blockIndex];
  }

  console.log(sparkline);
  console.log('Min: ' + stats.min + ' | Max: ' + stats.max + ' | Points: ' + keys.length);
  console.log('');
}

function printSummary(dataObj) {
  const keys = Object.keys(dataObj);
  const stats = getMinMax(dataObj);

  console.log('\nSummary:\n');
  console.log('Points: ' + keys.length);
  console.log('Y Range: ' + stats.min + ' - ' + stats.max);
  console.log('X Range: ' + stats.xMin + ' - ' + stats.xMax);

  let sum = 0;
  for (let i = 0; i < keys.length; i = i + 1) {
    sum = sum + dataObj[keys[i]];
  }
  const avg = sum / keys.length;
  console.log('Average: ' + avg.toFixed(2));
  console.log('');

  console.log('Data Points:');
  for (let i = 0; i < keys.length; i = i + 1) {
    const key = keys[i];
    const value = dataObj[key];
    const bar = config.barChar.repeat(Math.floor(value));
    console.log('  ' + key.padStart(3, ' ') + ' | ' + bar + ' (' + value + ')');
  }
}

printSummary(data);
drawSparkline(data);
drawLineGraph(data);
drawBarChart(data);
