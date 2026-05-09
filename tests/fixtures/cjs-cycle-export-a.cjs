class CycleExportA {
  static applyToClass() {
    return 'ok';
  }
}

module.exports = CycleExportA;

const b = require('./cjs-cycle-export-b.cjs');
module.exports.seenType = b.seenType;
module.exports.seenValue = b.seenValue;
