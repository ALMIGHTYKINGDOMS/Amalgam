// Release hash manifest + inventory generator.
//
// Walks a release directory and writes:
//   SHA256SUMS.txt      – "sha256  relative/path" per file (sorted, stable)
//   inventory.json      – [{ name, size, sha256, mtime }]
//
// Usage:
//   node tools/hash-manifest.mjs <release-dir> [output-dir]
//
// Deterministic output: paths sorted, hashes lower-case hex. Exits 1 when
// the input directory is missing or contains no files.

import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";

const dir = path.resolve(process.argv[2] || "");
const outDir = path.resolve(process.argv[3] || dir);

if (!dir || !fs.existsSync(dir) || !fs.statSync(dir).isDirectory()) {
  console.error("release directory not found: " + dir);
  process.exit(1);
}

function walk(base, rel) {
  const results = [];
  for (const name of fs.readdirSync(path.join(base, rel))) {
    const full = path.join(base, rel, name);
    const st = fs.statSync(full);
    if (st.isDirectory()) results.push(...walk(base, path.join(rel, name)));
    else if (st.isFile()) results.push({ rel: path.join(rel, name), st });
  }
  return results;
}

const files = walk(dir, "").sort((a, b) => a.rel.localeCompare(b.rel));
if (files.length === 0) {
  console.error("no files found in release directory");
  process.exit(1);
}

const inventory = [];
const sums = [];
let total = 0n;
for (const { rel, st } of files) {
  const hash = crypto.createHash("sha256");
  hash.update(fs.readFileSync(path.join(dir, rel)));
  const sha256 = hash.digest("hex");
  const size = st.size;
  total += BigInt(size);
  sums.push(`${sha256}  ${rel.replace(/\\/g, "/")}`);
  inventory.push({ name: rel.replace(/\\/g, "/"), size, sha256, mtime: st.mtime.toISOString() });
}

fs.writeFileSync(path.join(outDir, "SHA256SUMS.txt"), sums.join("\n") + "\n", "utf-8");
fs.writeFileSync(
  path.join(outDir, "inventory.json"),
  JSON.stringify({ generated_at: new Date().toISOString(), files: inventory }, null, 2) + "\n",
  "utf-8",
);

console.log(`Wrote SHA256SUMS.txt and inventory.json (${files.length} files, ${total} bytes) to ${outDir}`);
