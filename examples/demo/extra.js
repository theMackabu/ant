const rgbToHex = (r, g, b) => '#' + ((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1);

const date = {
  diff: (date1, date2) => Math.ceil(Math.abs(date1.getTime() - date2.getTime()) / 86400000),
  isWeek: date => date.getDay() % 6 !== 0,
  timeFrom: date => date.toTimeString().slice(0, 8)
};

const math = {
  randInt: (min = 0, max = 1) => Math.floor(Math.random() * (max - min + 1)) + min,
  randFloat: (min = 0, max = 1) => Math.random() * (max - min + 1) + min
};

const temp = {
  ctof: celsius => (celsius * 9) / 5 + 32,
  ftoc: fahrenheit => ((fahrenheit - 32) * 5) / 9
};

String.prototype.reverse = function () {
  return [...this].reverse().join('');
};

String.prototype.json = function () {
  return JSON.parse(this);
};

console.log(rgbToHex(255, 0, 244));
console.log(date.timeFrom(new Date()));
console.log(date.diff(new Date('2020-10-21'), new Date()));
console.log(date.isWeek(new Date()));
console.log(math.randInt(0, 5));
console.log(math.randFloat(0, 5));
console.log(temp.ctof(15));
console.log(temp.ftoc(15));

console.log('hello world'.reverse());
console.log('{"hello":"world"}'.json()['hello']);
