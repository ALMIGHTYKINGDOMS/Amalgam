import test from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const script = path.join(import.meta.dirname, "secret-scan.mjs");

test("scans every supplied root after normalized deduplication", (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-secret-scan-"));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));

  const firstRoot = path.join(dir, "first-root");
  const laterRoot = path.join(dir, "later-root");
  fs.mkdirSync(firstRoot);
  fs.mkdirSync(laterRoot);
  fs.writeFileSync(path.join(firstRoot, "clean.txt"), "nothing secret-shaped here\n");

  // This is a header only, not credential material. Build it at runtime so
  // the repository itself does not carry a scanner-triggering literal.
  const marker = ["-----BEGIN", "PRIVATE", "KEY-----"].join(" ");
  fs.writeFileSync(path.join(laterRoot, "scan-probe.txt"), `${marker}\n`);

  const result = spawnSync(process.execPath, [
    script,
    firstRoot,
    laterRoot,
    `${laterRoot}${path.sep}.`,
  ], { encoding: "utf8" });

  assert.ifError(result.error);
  assert.equal(result.status, 1, result.stdout + result.stderr);
  assert.match(result.stdout, /Scanned 2 text files/);
  assert.match(result.stdout, /Private RSA \/ EC key/);
  assert.match(result.stdout, /scan-probe\.txt/);
  assert.equal(result.stdout.includes(marker), false, "scanner must not print a match");
});

test("defaults to the current directory when no roots are supplied", (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-secret-scan-default-"));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  fs.writeFileSync(path.join(dir, "clean.txt"), "safe text\n");

  const result = spawnSync(process.execPath, [script], {
    cwd: dir,
    encoding: "utf8",
  });

  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /Scanned 1 text files/);
  assert.match(result.stdout, /No secret-category matches/);
});
