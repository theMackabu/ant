function mandelbrot(center_x, center_y, scale, w, h, max_it) {
  const colors = [14, 15, 7, 8, 0, 4, 12, 5, 13, 1, 9, 3, 11, 10, 2, 6];
  const numColors = colors.length;
  const fx = (scale * 0.5) / Math.min(w, h);
  const fy = fx * 2;
  const halfW = w * 0.5;
  const halfH = h * 0.5;

  const cxArr = new Float64Array(w);
  for (let x1 = 0; x1 < w; x1++) {
    cxArr[x1] = (x1 - halfW) * fx + center_x;
  }

  function iterate(cx, cy) {
    const q = (cx - 0.25) * (cx - 0.25) + cy * cy;
    if (q * (q + (cx - 0.25)) <= 0.25 * cy * cy) return max_it;

    const cx1 = cx + 1;
    if (cx1 * cx1 + cy * cy <= 0.0625) return max_it;

    let x = 0,
      y = 0,
      x2 = 0,
      y2 = 0;
    let i = 0;

    while (i < max_it && x2 + y2 < 4) {
      y = 2 * x * y + cy;
      x = x2 - y2 + cx;
      x2 = x * x;
      y2 = y * y;
      i++;
    }
    return i;
  }

  const output = [];

  for (let y1 = 0; y1 < h; y1++) {
    const cy0 = (y1 - halfH) * fy + center_y;
    const cy1 = (y1 + 0.5 - halfH) * fy + center_y;
    const chars = new Array(w + 1);

    for (let x1 = 0; x1 < w; x1++) {
      const cx = cxArr[x1];

      const i0 = iterate(cx, cy0);
      const i1 = iterate(cx, cy1);

      const c0 = i0 >= max_it ? 0 : colors[i0 % numColors];
      const c1 = i1 >= max_it ? 0 : colors[i1 % numColors];

      const fg = c0 >= 8 ? 82 + c0 : 30 + c0;
      const bg = c1 >= 8 ? 92 + c1 : 40 + c1;

      chars[x1] = '\x1b[' + fg + ';' + bg + 'm\u2580';
    }
    chars[w] = '\x1b[0m';
    output.push(chars.join(''));
  }

  console.log(output.join('\n'));
}

mandelbrot(-0.75, 0.0, 2.0, 80, 25, 50);
