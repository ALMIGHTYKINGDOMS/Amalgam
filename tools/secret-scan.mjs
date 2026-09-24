// Secret-category scanner for Amalgam.
//
// Scans one or more paths (default: repository root) for secret-like material across
// several categories. NEVER prints the matched value: only the category,
// location, whether the hit is expected, and the recommended action.
//
// Usage:
//   node tools/secret-scan.mjs [path ...]
//
// Exit code 0 = no HIGH findings; 1 = HIGH findings present.

import fs from "node:fs";
import path from "node:path";

const rootArgs = process.argv.slice(2);
const roots = [...new Set(
  (rootArgs.length ? rootArgs : [process.cwd()])
    .map((root) => path.resolve(root))
)];

// Categories: regex, severity, expected-locations note.
const CATEGORIES = [
  {
    name: "Supabase service_role key",
    severity: "HIGH",
    re: /eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,}/,
  },
  {
    name: "Private RSA / EC key",
    severity: "HIGH",
    re: /-----BEGIN (RSA |EC |OPENSSH |)PRIVATE KEY-----/,
  },
  {
    name: "PFX / PKCS#12 certificate",
    severity: "HIGH",
    re: /-----BEGIN (PKCS12|ENCRYPTED PRIVATE KEY)-----/,
  },
  {
    name: "Whop / payment secret",
    severity: "HIGH",
    re: /\b(whop|wh_|paypal|stripe)[-_]?(secret|sk|key|token)[-_]?[a-z0-9_]*\s*[:=]/i,
  },
  {
    name: "TURN master secret",
    severity: "HIGH",
    re: /\b(cf_turn|turn_api_token|turn_secret|rtc_)[a-z0-9_]*\s*[:=]\s*["']?[A-Za-z0-9_\-]{16,}/i,
  },
  {
    name: "OAuth / generic API secret assignment",
    severity: "MEDIUM",
    re: /\b(client_secret|api_secret|secret_key|access_token|api_token)\s*[:=]\s*["'][A-Za-z0-9_\-]{12,}["']/i,
  },
  {
    name: "Database connection with password",
    severity: "HIGH",
    re: /(?:postgres|mysql|mssql|mongodb)(?:\+ssl)?:\/\/[^:\/\s]+:[^@\s]+@/i,
  },
  {
    name: "AWS-style access key",
    severity: "HIGH",
    re: /\bAKIA[0-9A-Z]{16}\b/,
  },
];

const SKIP_DIRS = new Set([
  "node_modules", ".git", "build", "build-quick", "build-verify", "dist",
  "installer", "_pgcheck", ".temp", "bin", "obj", ".gradle",
]);
const SKIP_EXT = new Set([".png", ".jpg", ".jpeg", ".gif", ".ico", ".zip", ".jar",
  ".exe", ".dll", ".db", ".sqlite", ".sqlite3", ".obj", ".pdb", ".hprof", ".woff", ".ttf"]);
const MAX_FILE_BYTES = 2 * 1024 * 1024;

function walk(dir, out) {
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const e of entries) {
    if (e.name.startsWith(".") && SKIP_DIRS.has(e.name)) continue;
    if (SKIP_DIRS.has(e.name)) continue;
    const full = path.join(dir, e.name);
    if (e.isDirectory()) walk(full, out);
    else if (e.isFile() && !SKIP_EXT.has(path.extname(e.name).toLowerCase())) {
      out.push(full);
    }
  }
}

// Known test fixtures that intentionally contain secret-shaped material.
// Vendored third-party tests and literal test strings are expected; matches
// here are reported but never fail the scan.
// Paths use either separator on Windows; match both.
const SEP = /[\\/]/;
const EXPECTED_FIXTURES = [
  { re: new RegExp(SEP.source + "vendor" + SEP.source + "libdatachannel" + SEP.source + "test" + SEP.source), why: "upstream WebRTC test certificates" },
  { re: new RegExp(SEP.source + "cpp" + SEP.source + "tests" + SEP.source), why: "unit-test literal values like test_access_token_value" },
  { re: /\.test\.mjs$/, why: "tooling test fixtures" },
];

const findings = [];
let scanned = 0;
const files = [];
for (const root of roots) walk(root, files);

for (const file of new Set(files)) {
  let stat;
  try {
    stat = fs.statSync(file);
  } catch {
    continue;
  }
  if (stat.size > MAX_FILE_BYTES) continue;
  let text;
  try {
    text = fs.readFileSync(file, "utf-8");
  } catch {
    continue; // binary
  }
  if (!text) continue;
  scanned++;
  for (const cat of CATEGORIES) {
    if (cat.re.test(text)) {
      findings.push({
        type: cat.name,
        severity: cat.severity,
        location: path.relative(process.cwd(), file),
      });
    }
  }
}

// Deduplicate (same category + file).
const seen = new Set();
const unique = findings.filter((f) => {
  const k = f.type + "|" + f.location;
  if (seen.has(k)) return false;
  seen.add(k);
  return true;
});

const high = unique.filter((f) => f.severity === "HIGH");
const unexpected = high.filter((f) => !EXPECTED_FIXTURES.some((fx) => fx.re.test(f.location)));
console.log(`Scanned ${scanned} text files under ${roots.join(", ")}`);
if (unique.length === 0) {
  console.log("No secret-category matches.");
} else {
  for (const f of unique.sort((a, b) => a.location.localeCompare(b.location))) {
    const expected = EXPECTED_FIXTURES.some((fx) => fx.re.test(f.location))
      ? " [EXPECTED FIXTURE]"
      : "";
    console.log(`[${f.severity}] ${f.type}  ->  ${f.location}${expected}`);
  }
}
console.log(`Total: ${unique.length} (${high.length} HIGH, ${unexpected.length} unexpected HIGH)`);
process.exit(unexpected.length > 0 ? 1 : 0);
