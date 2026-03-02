let failed = 0;

function check(name, cond, detail) {
  if (!cond) {
    failed++;
    console.log("FAIL", name, detail || "");
  }
}

const s = "false — must be >= 0";
const p = s.padEnd(25);
check("padEnd length with em dash", p.length === 25, p.length);
check("padEnd content with em dash", p === "false — must be >= 0     ", JSON.stringify(p));

const p2 = s.padStart(25);
check("padStart length with em dash", p2.length === 25, p2.length);
check("padStart content with em dash", p2 === "     false — must be >= 0", JSON.stringify(p2));

const p3 = "ab".padEnd(5, "—");
check("padEnd unicode pad string", p3 === "ab———", JSON.stringify(p3));
check("padEnd unicode pad length", p3.length === 5, p3.length);

if (failed > 0) throw new Error("test_string_pad_unicode failed");
