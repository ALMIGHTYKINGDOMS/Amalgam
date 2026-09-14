#!/usr/bin/env node
// Update-check end-to-end proof against the shipped exe:
//   1. signed feed over loopback HTTP (env-gated seam) -> accepted
//   2. tampered manifest (field flipped)                -> rejected
//   3. manifest signed with the WRONG key               -> rejected
//   4. loopback HTTP without the env flag               -> rejected (fail-closed seam)
// Usage: node tmp-update-e2e.mjs <exePath> <feedDir> [port]
import { spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { canonicalize } from "./update-manifest.mjs";

const [exe, feedDir] = process.argv.slice(2);
const port = Number(process.argv[4] ?? 18091);
if (!exe || !fs.existsSync(exe)) { console.error(`exe missing: ${exe}`); process.exit(2); }

// Serve a caller-supplied object of {name: bytes} so tampered variants never
// touch the real feed directory.
function serve(files) {
  return new Promise((resolve) => {
    const srv = http.createServer((req, res) => {
      const name = req.url.split("?")[0].replace(/^\//, "") || "manifest.json";
      const body = files[name];
      if (!body) { res.writeHead(404); res.end("nf"); return; }
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(body);
    });
    srv.listen(port, "127.0.0.1", () => resolve(() => new Promise((done) => srv.close(done))));
  });
}

function checkUpdate(url, withEnv) {
  const env = { ...process.env };
  if (withEnv) env.AMALGAM_TEST_LOOPBACK_HTTP = "1";
  else delete env.AMALGAM_TEST_LOOPBACK_HTTP;
  // The GUI-subsystem exe produces no output under spawnSync pipe capture;
  // spawn + explicit stream drain is the pattern that works.
  return new Promise((resolve) => {
    const p = spawn(exe, ["--check-update", url], { env });
    let out = "";
    const t = setTimeout(() => p.kill(), 60000);
    p.stdout.on("data", (d) => (out += d));
    p.stderr.on("data", (d) => (out += d));
    p.on("exit", (code) => { clearTimeout(t); resolve({ code, out: out.trim() }); });
  });
}

const manifestBytes = fs.readFileSync(path.join(feedDir, "manifest.json"));
const url = `http://127.0.0.1:${port}/manifest.json`;
const results = [];
const report = (name, pass, detail) => { results.push({ name, pass }); console.log(`${pass ? "PASS" : "FAIL"} ${name}${detail ? ` — ${detail}` : ""}`); };

// Case 1: the real signed feed.
let stop = await serve({ "manifest.json": manifestBytes });
{
  const r = await checkUpdate(url, true);
  const ok = r.code === 0 && /UPDATE-CHECK OK/.test(r.out);
  report("signed feed accepted", ok, r.out.split("\n")[0]);
}
await stop();

// Case 2: tamper a signed field (size+1 invalidates the signature).
{
  const m = JSON.parse(manifestBytes.toString("utf8"));
  const tampered = Buffer.from(JSON.stringify({ ...m, size: Number(m.size) + 1 }));
  stop = await serve({ "manifest.json": tampered });
  const r = await checkUpdate(url, true);
  report("tampered manifest rejected", r.code !== 0 && /UPDATE-CHECK REJECTED/.test(r.out), r.out.split("\n")[0]);
  await stop();
}

// Case 3: well-formed manifest signed by an attacker's key.
{
  const m = JSON.parse(manifestBytes.toString("utf8"));
  const { privateKey } = crypto.generateKeyPairSync("rsa", { modulusLength: 2048 });
  // Sign the REAL canonical byte contract (same as tools/update-manifest.mjs)
  // and replace the field the launcher actually verifies: manifest_signature.
  const sig = crypto.sign("sha256", Buffer.from(canonicalize(m)), { key: privateKey, padding: crypto.constants.RSA_PKCS1_PADDING });
  const hostile = Buffer.from(JSON.stringify({ ...m, manifest_signature: sig.toString("base64") }));
  stop = await serve({ "manifest.json": hostile });
  const r = await checkUpdate(url, true);
  report("wrong-key signature rejected", r.code !== 0 && /UPDATE-CHECK REJECTED/.test(r.out), r.out.split("\n")[0]);
  await stop();
}

// Case 4: loopback HTTP must stay fail-closed without the env flag.
{
  stop = await serve({ "manifest.json": manifestBytes });
  const r = await checkUpdate(url, false);
  report("loopback http without env flag rejected", r.code !== 0 && /only https/.test(r.out), r.out.split("\n")[0]);
  await stop();
}

const failed = results.filter((x) => !x.pass);
console.log(`\n${results.length - failed.length}/${results.length} e2e checks passed`);
process.exit(failed.length ? 1 : 0);
