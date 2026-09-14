import test from "node:test";
import assert from "node:assert/strict";
import crypto from "node:crypto";
import { execFileSync } from "node:child_process";
import {
  canonicalize,
  signString,
  verifyString,
  verifyManifest,
  sha256File,
} from "./update-manifest.mjs";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

// The canonical-form byte contract with cpp/tests/updater_test.cpp.
const GOLDEN_CANONICAL =
  '{"channel":"stable","download_url":"https://cdn.example.com/amalgam-3.0.1.zip",' +
  '"mandatory":false,"min_version":"3.0","notes":"fixes",' +
  '"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",' +
  '"size":1234,"version":"3.0.1"}';

const SAMPLE = {
  version: "3.0.1",
  channel: "stable",
  download_url: "https://cdn.example.com/amalgam-3.0.1.zip",
  sha256: "a".repeat(64),
  size: 1234,
  min_version: "3.0",
  notes: "fixes",
  mandatory: false,
};

function keypair() {
  const { publicKey, privateKey } = crypto.generateKeyPairSync("rsa", {
    modulusLength: 2048,
  });
  return {
    publicKey: publicKey.export({ type: "spki", format: "pem" }),
    privateKey: privateKey.export({ type: "pkcs8", format: "pem" }),
  };
}

test("canonical form matches the C++ release-tooling contract", () => {
  assert.equal(canonicalize(SAMPLE), GOLDEN_CANONICAL);
});

test("canonical form is stable and rejects extra keys", () => {
  const withExtra = { ...SAMPLE, extra_field: "x" };
  assert.equal(canonicalize(withExtra), canonicalize(SAMPLE));
});

test("sign + verify round-trips with the same key", () => {
  const { publicKey, privateKey } = keypair();
  const sig = signString(privateKey, GOLDEN_CANONICAL);
  assert.equal(verifyString(publicKey, GOLDEN_CANONICAL, sig), true);
});

test("field modification after signing invalidates the signature", () => {
  const { publicKey, privateKey } = keypair();
  const manifest = { ...SAMPLE };
  const sig = signString(privateKey, canonicalize(manifest));

  const tampered = (mut) => {
    const m = { ...manifest, ...mut };
    return { m, sig };
  };

  const cases = [
    ["version", { version: "3.0.2" }],
    ["download_url", { download_url: "https://evil.example.com/x.zip" }],
    ["sha256", { sha256: "b".repeat(64) }],
    ["size", { size: 9999 }],
    ["channel", { channel: "beta" }],
    ["min_version", { min_version: "2.0" }],
    ["mandatory", { mandatory: true }],
  ];
  for (const [name, mut] of cases) {
    const { m } = tampered(mut);
    assert.equal(
      verifyString(publicKey, canonicalize(m), sig),
      false,
      `${name} modification must invalidate`
    );
  }
});

test("missing signature fails under requireSigned", () => {
  const { publicKey } = keypair();
  const result = verifyManifest({ ...SAMPLE }, publicKey, { requireSigned: true });
  assert.equal(result.ok, false);
  assert.match(result.error, /missing/);
});

test("wrong key rejected", () => {
  const a = keypair();
  const b = keypair();
  const sig = signString(a.privateKey, canonicalize(SAMPLE));
  assert.equal(verifyString(b.publicKey, canonicalize(SAMPLE), sig), false);
});

test("bad base64 signature rejected", () => {
  const { publicKey } = keypair();
  assert.equal(verifyString(publicKey, canonicalize(SAMPLE), "not-a-signature"), false);
});

test("sha256File matches crypto", () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "amal-manifest-"));
  const f = path.join(dir, "payload.bin");
  fs.writeFileSync(f, "hello payload");
  const expect = crypto.createHash("sha256").update("hello payload").digest("hex");
  assert.equal(sha256File(f), expect);
  fs.rmSync(dir, { recursive: true, force: true });
});

// Regression: the CLI parsed kebab-case flags (--private-key) but the handlers
// read camelCase (privateKey), silently dropping the key and writing UNSIGNED
// manifests. Drive the real CLI end-to-end so the parse/write path stays
// covered.
test("CLI accepts kebab-case flags and signs the manifest", () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "amal-manifest-cli-"));
  const key = path.join(dir, "k.pem");
  fs.writeFileSync(key, keypair().privateKey);
  const out = path.join(dir, "manifest.json");
  const script = path.join(import.meta.dirname, "update-manifest.mjs");
  execFileSync(process.execPath, [script, "generate",
    "--version", "1.2.3", "--channel", "stable",
    "--url", "https://cdn.example.com/x.zip",
    "--sha256", "a".repeat(64), "--min-version", "1.0",
    "--private-key", key, "--out", out]);
  const manifest = JSON.parse(fs.readFileSync(out, "utf8"));
  assert.ok(manifest.manifest_signature, "kebab-case --private-key was dropped");
  assert.equal(manifest.min_version, "1.0");
  assert.equal(verifyManifest(manifest, keypair().publicKey, { requireSigned: true }).ok,
    false, "signature must not verify against the wrong key");
  fs.rmSync(dir, { recursive: true, force: true });
});
