let failed = 0;
const eq = (n, a, e) => { const ok = Object.is(a, e); if (!ok) failed++; console.log(`${ok?'PASS':'FAIL'} ${n}${ok?'':` (got ${a}, want ${e})`}`); };

// --- stale-IC check: warm a site, then wholesale-reassign String.prototype.
// ant makes String.prototype writable (documented divergence); node ignores the
// write. Detect which took effect and assert the IC agrees with reality —
// a stale IC would keep returning the old method's result either way.
function callIt(s) { return s.charCodeAt(0); }
for (let i = 0; i < 2000; i++) callIt("abc");
eq('warm charCodeAt', callIt("abc"), 97);

const savedProto = String.prototype;
const replacement = { charCodeAt() { return -1; } };
String.prototype = replacement;
const took = "x".charCodeAt === replacement.charCodeAt;
eq('reassigned proto observed by warm site', callIt("abc"), took ? -1 : 97);
String.prototype = savedProto;
eq('restored proto observed', callIt("abc"), 97);

// --- method replacement via shape transition on the real prototype
function up(s) { return s.toUpperCase(); }
const origUpper = String.prototype.toUpperCase;
for (let i = 0; i < 2000; i++) up("xy");
eq('warm toUpperCase', up("xy"), "XY");
String.prototype.toUpperCase = function () { return "SWAPPED"; };
eq('replaced method observed by warm site', up("xy"), "SWAPPED");
String.prototype.toUpperCase = origUpper;
eq('restored method observed', up("xy"), "XY");

// --- in-place overwrite (sv_try_put_field_fast path: no shape change)
String.prototype.toUpperCase = function () { return "SWAP2"; };
eq('in-place replaced observed', up("xy"), "SWAP2");
String.prototype.toUpperCase = origUpper;
eq('in-place restored observed', up("xy"), "XY");

// --- polymorphic site: same IC fed string AND object receivers
function len(x) { return x.length; }
const objWithLen = { length: 42 };
for (let i = 0; i < 3000; i++) { len("hello"); len(objWithLen); }
eq('poly string length', len("hello"), 5);
eq('poly object length', len(objWithLen), 42);

// --- number/boolean/string through one shared site
function ts(x) { return x.toString(); }
for (let i = 0; i < 2000; i++) { ts(7); ts(true); ts("s"); }
eq('num toString', ts(7), "7");
eq('bool toString', ts(true), "true");
eq('str toString', ts("s"), "s");

// --- string index keys must not go through the prim IC
function idx(s) { return s[1]; }
for (let i = 0; i < 2000; i++) idx("abc");
eq('string index access', idx("abc"), "b");

// --- negative-cache invalidation: a warmed MISS site must observe late additions
function probe(s) { return s.lateAddedMethod === undefined ? -1 : s.lateAddedMethod(); }
for (let i = 0; i < 3000; i++) probe("zz");
eq('warm miss site', probe("zz"), -1);
String.prototype.lateAddedMethod = function () { return 77; };      // shape1 guard
eq('String.prototype addition observed', probe("zz"), 77);
delete String.prototype.lateAddedMethod;
eq('deletion observed', probe("zz"), -1);

// A computed-key insertion can mutate a detached prototype shape in place.
// The negative IC must not treat an unchanged shape pointer as proof that the
// property is still absent.
const detachKey = 'lateComputedDetach';
String.prototype[detachKey] = 1;
delete String.prototype[detachKey];

function probeComputed(s) { return s.lateComputedProp; }
for (let i = 0; i < 3000; i++) probeComputed("zz");
eq('warm computed-key miss site', probeComputed("zz"), undefined);
const computedKey = ['late', 'Computed', 'Prop'].join('');
String.prototype[computedKey] = 7;
eq('computed prototype addition visible cold', "zz"[computedKey], 7);
eq('computed prototype addition observed by warm site', probeComputed("zz"), 7);
delete String.prototype[computedKey];
eq('computed prototype deletion observed', probeComputed("zz"), undefined);

function probe2(s) { return s.lateObjProtoProp === undefined ? -1 : s.lateObjProtoProp; }
for (let i = 0; i < 3000; i++) probe2("zz");
eq('warm miss site 2', probe2("zz"), -1);
Object.prototype.lateObjProtoProp = 88;                             // shape2 guard
eq('Object.prototype addition observed', probe2("zz"), 88);
delete Object.prototype.lateObjProtoProp;
eq('obj proto deletion observed', probe2("zz"), -1);

const objDetachKey = 'lateObjComputedDetach';
Object.prototype[objDetachKey] = 1;
delete Object.prototype[objDetachKey];

function probeObjComputed(s) { return s.lateObjComputedProp; }
for (let i = 0; i < 3000; i++) probeObjComputed("zz");
eq('warm second-prototype computed-key miss', probeObjComputed("zz"), undefined);
const objComputedKey = ['late', 'Obj', 'Computed', 'Prop'].join('');
Object.prototype[objComputedKey] = 91;
eq('second-prototype computed addition visible cold', "zz"[objComputedKey], 91);
eq('second-prototype computed addition observed warm', probeObjComputed("zz"), 91);
delete Object.prototype[objComputedKey];
eq('second-prototype computed deletion observed', probeObjComputed("zz"), undefined);

// accessor added to the proto after a warm miss: the warmed site must agree
// with a cold access. (Whether proto accessors fire for primitive receivers at
// all is a separate, pre-existing engine gap — node returns the getter value,
// ant currently returns undefined on every binary; tracked in tech-debt. The
// invariant pinned here is only that the IC never serves a stale answer.)
function probe3(s) { return s.lateAccessor; }
for (let i = 0; i < 3000; i++) probe3("zz");
eq('warm miss site 3', probe3("zz"), undefined);
Object.defineProperty(String.prototype, 'lateAccessor', { get() { return 99; }, configurable: true });
const coldAccessor = ("z" + "z").lateAccessor;
eq('warm site agrees with cold access', probe3("zz"), coldAccessor);
delete String.prototype.lateAccessor;
eq('accessor deletion observed', probe3("zz"), undefined);

if (failed) { console.log(`${failed} FAILED`); process.exit(1); }
console.log('OK');
