#!/usr/bin/env node
// Amalgam launcher update-manifest tooling (release side).
//
// Generates a deterministic update manifest whose security-relevant fields
// (version, channel, download_url, sha256, size, min_version) are bound by an
// RSA-SHA256 signature over a CANONICAL JSON form. The C++ updater
// (launcher/src/updater.cpp) recomputes the same canonical bytes from the
// parsed fields and verifies the signature, so mutating any field
// invalidates the manifest. Payload bytes are signed separately
// (sign-payload) and verified by the updater over the staged file.
//
// The canonical form is the byte contract with updater.cpp:
//   {"channel":..,"download_url":..,"mandatory":..,"min_version":..,
//    "notes":..,"sha256":..,"size":..,"version":..}
// (fixed key order, compact, JSON.stringify escaping — identical to
// Json::dump in json.cpp for the same object).
//
// Private keys are NEVER part of the repository; they come from CI secrets
// or a local release vault. Public keys may ship with the launcher.

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

// Keep this in lockstep with the launcher's update parser. A release tool
// must never sign a manifest the launcher is guaranteed to reject.
export const MAX_PAYLOAD_BYTES = 32 * 1024 * 1024 * 1024;

const FIELD_ORDER = [
  "channel",
  "download_url",
  "mandatory",
  "min_version",
  "notes",
  "sha256",
  "size",
  "version",
];

/** Deterministic canonical JSON for the security-relevant manifest fields. */
export function canonicalize(manifest) {
  const ordered = {};
  for (const key of FIELD_ORDER) ordered[key] = manifest[key];
  // JSON.stringify: compact, insertion order preserved, matches Json::dump.
  return JSON.stringify(ordered);
}

/** SHA-256 hex digest of a file (used for the payload hash field). */
export function sha256File(filePath) {
  return crypto.createHash("sha256").update(fs.readFileSync(filePath)).digest("hex");
}

/** Base64 RSA-SHA256 (PKCS#1 v1.5) signature over a UTF-8 string. */
export function signString(pemPrivateKey, data) {
  return crypto
    .sign("sha256", Buffer.from(data, "utf-8"), pemPrivateKey)
    .toString("base64");
}

/** Base64 RSA-SHA256 signature over raw file bytes. */
export function signFile(pemPrivateKey, filePath) {
  return crypto
    .sign("sha256", fs.readFileSync(filePath), pemPrivateKey)
    .toString("base64");
}

/** Verify a base64 RSA-SHA256 signature over raw file bytes. */
export function verifyFile(pemPublicKey, filePath, signatureB64) {
  try {
    return crypto.verify(
      "sha256",
      fs.readFileSync(filePath),
      pemPublicKey,
      Buffer.from(signatureB64, "base64")
    );
  } catch {
    return false;
  }
}

/** Return a precise validity result for an update payload size. */
export function validatePayloadSize(value) {
  const size = Number(value);
  if (!Number.isSafeInteger(size) || size <= 0 || size > MAX_PAYLOAD_BYTES) {
    return { ok: false, size: null, error: `size must be a positive integer no greater than ${MAX_PAYLOAD_BYTES}` };
  }
  return { ok: true, size, error: null };
}

/** Verify a manifest's declared payload identity against a local final ZIP. */
export function verifyPayload(manifest, filePath, pemPublicKey, {
  requirePayloadSignature = true,
} = {}) {
  if (!filePath || !fs.existsSync(filePath) || !fs.statSync(filePath).isFile()) {
    return { ok: false, error: "payload file is missing" };
  }
  const sizeResult = validatePayloadSize(manifest?.size);
  if (!sizeResult.ok) return sizeResult;
  const actualSize = fs.statSync(filePath).size;
  if (actualSize !== sizeResult.size) {
    return { ok: false, error: `payload size mismatch: manifest=${sizeResult.size} actual=${actualSize}` };
  }
  if (!/^[a-f0-9]{64}$/i.test(manifest?.sha256 || "")) {
    return { ok: false, error: "manifest payload hash is malformed" };
  }
  const actualHash = sha256File(filePath);
  if (actualHash !== manifest.sha256.toLowerCase()) {
    return { ok: false, error: "payload hash mismatch" };
  }
  if (!manifest.signature) {
    return requirePayloadSignature
      ? { ok: false, error: "missing payload signature" }
      : { ok: true, error: null };
  }
  if (!verifyFile(pemPublicKey, filePath, manifest.signature)) {
    return { ok: false, error: "payload signature verification failed" };
  }
  return { ok: true, error: null };
}

/** Verify a base64 RSA-SHA256 signature over a string. Returns bool. */
export function verifyString(pemPublicKey, data, signatureB64) {
  try {
    return crypto.verify(
      "sha256",
      Buffer.from(data, "utf-8"),
      pemPublicKey,
      Buffer.from(signatureB64, "base64")
    );
  } catch {
    return false;
  }
}

/** Verify a manifest's manifest_signature against its canonical fields. */
export function verifyManifest(manifest, pemPublicKey, { requireSigned = true } = {}) {
  if (!manifest.manifest_signature) {
    return requireSigned
      ? { ok: false, error: "missing manifest signature" }
      : { ok: true, error: null };
  }
  const ok = verifyString(pemPublicKey, canonicalize(manifest), manifest.manifest_signature);
  return ok
    ? { ok: true, error: null }
    : { ok: false, error: "manifest signature verification failed" };
}

function usage() {
  console.log(`Usage:
  node update-manifest.mjs generate --version V --channel CH --url URL (--file payload.zip | --sha256 H --size N) [--min-version MV] [--notes TEXT] [--mandatory] --private-key KEY.pem [--out manifest.json] [--allow-unsigned-dev]
  node update-manifest.mjs sign-payload --manifest manifest.json --file payload.zip --private-key KEY.pem [--out manifest.json]
  node update-manifest.mjs verify --manifest manifest.json --public-key PUB.pem [--payload payload.zip] [--require-payload-signature] [--allow-unsigned-dev]
  node update-manifest.mjs hash --file PATH`);
}

function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i += 2) {
    const key = argv[i];
    // Normalize kebab-case flags (--private-key) to the camelCase names the
    // command handlers read. A mismatch here silently dropped the signing
    // key and produced unsigned manifests.
    const name = key.slice(2).replace(/-([a-z0-9])/g, (_, c) => c.toUpperCase());
    const val = argv[i + 1];
    if (key === "--mandatory" || key === "--require-signed" ||
        key === "--require-payload-signature" || key === "--allow-unsigned-dev") {
      args[name] = true;
      i -= 1;
    } else {
      args[name] = val;
    }
  }
  return args;
}

function readKey(file) {
  if (!file) return null;
  const pem = fs.readFileSync(file, "utf-8");
  // Private keys must never be echoed; just validate they parse.
  crypto.createPrivateKey(pem);
  return pem;
}

function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  if (!cmd || cmd === "--help" || cmd === "-h") return usage();
  const a = parseArgs(rest);

  if (cmd === "hash") {
    console.log(sha256File(a.file));
    return;
  }

  if (cmd === "generate") {
    if (!a.version || !a.channel || !a.url) {
      console.error("generate requires --version --channel --url and a payload identity");
      process.exit(2);
    }
    if (a.file) {
      if (!fs.existsSync(a.file) || !fs.statSync(a.file).isFile()) {
        console.error("--file must name an existing payload file");
        process.exit(2);
      }
      const derivedHash = sha256File(a.file);
      const derivedSize = fs.statSync(a.file).size;
      if (a.sha256 && a.sha256.toLowerCase() !== derivedHash) {
        console.error("--sha256 does not match --file");
        process.exit(2);
      }
      if (a.size !== undefined && Number(a.size) !== derivedSize) {
        console.error("--size does not match --file");
        process.exit(2);
      }
      a.sha256 = derivedHash;
      a.size = String(derivedSize);
    }
    if (!a.sha256 || a.size === undefined) {
      console.error("generate requires --file or both --sha256 and --size");
      process.exit(2);
    }
    if (!/^[a-f0-9]{64}$/i.test(a.sha256)) {
      console.error("--sha256 must be a 64-character hex digest");
      process.exit(2);
    }
    const sizeResult = validatePayloadSize(a.size);
    if (!sizeResult.ok) {
      console.error(`--${sizeResult.error}`);
      process.exit(2);
    }
    if (a.url && !a.url.startsWith("https://")) {
      console.error("--url must be https://");
      process.exit(2);
    }
    if (!a.privateKey && !a.allowUnsignedDev) {
      console.error("generate requires --private-key (use --allow-unsigned-dev only for local fixtures)");
      process.exit(2);
    }
    const manifest = {
      version: a.version,
      channel: a.channel,
      download_url: a.url,
      sha256: a.sha256.toLowerCase(),
      size: sizeResult.size,
      min_version: a.minVersion || "",
      notes: a.notes || "",
      mandatory: Boolean(a.mandatory),
    };
    const key = readKey(a.privateKey);
    if (key) {
      manifest.manifest_signature = signString(key, canonicalize(manifest));
    }
    // Fail closed: an explicitly requested signature must never silently
    // degrade to an unsigned manifest.
    if (a.privateKey && !manifest.manifest_signature) {
      console.error("signing failed; refusing to write an unsigned manifest");
      process.exit(2);
    }
    const out = a.out || "manifest.json";
    fs.writeFileSync(out, JSON.stringify(manifest, null, 2) + "\n");
    console.log(`wrote ${out}${key ? " (signed)" : " (unsigned development fixture)"}`);
    return;
  }

  if (cmd === "sign-payload") {
    const manifest = JSON.parse(fs.readFileSync(a.manifest, "utf-8"));
    const key = readKey(a.privateKey);
    if (!key) {
      console.error("sign-payload requires --private-key");
      process.exit(2);
    }
    const actual = sha256File(a.file);
    if (manifest.sha256 && actual !== manifest.sha256.toLowerCase()) {
      console.error(
        `payload hash mismatch: manifest=${manifest.sha256} actual=${actual}`
      );
      process.exit(2);
    }
    manifest.sha256 = actual;
    manifest.size = fs.statSync(a.file).size;
    const sizeResult = validatePayloadSize(manifest.size);
    if (!sizeResult.ok) {
      console.error(sizeResult.error);
      process.exit(2);
    }
    // Payload signature covers the raw file bytes; re-sign the manifest
    // because size/hash were pinned to the real artifact.
    manifest.signature = signFile(key, a.file);
    manifest.manifest_signature = signString(key, canonicalize(manifest));
    const out = a.out || a.manifest;
    fs.writeFileSync(out, JSON.stringify(manifest, null, 2) + "\n");
    console.log(`signed payload + manifest -> ${out}`);
    return;
  }

  if (cmd === "verify") {
    const manifest = JSON.parse(fs.readFileSync(a.manifest, "utf-8"));
    const pub = fs.readFileSync(a.publicKey, "utf-8");
    const result = verifyManifest(manifest, pub, {
      requireSigned: !a.allowUnsignedDev,
    });
    if (!result.ok) {
      console.error(`VERIFY FAIL: ${result.error}`);
      process.exit(1);
    }
    if (a.payload) {
      const payloadResult = verifyPayload(manifest, a.payload, pub, {
        requirePayloadSignature: !a.allowUnsignedDev || Boolean(a.requirePayloadSignature),
      });
      if (!payloadResult.ok) {
        console.error(`VERIFY FAIL: ${payloadResult.error}`);
        process.exit(1);
      }
      console.log("VERIFY OK (manifest + final payload signatures, hash, and size valid)");
    } else if (manifest.signature) {
      console.log("VERIFY OK (manifest signature valid; pass --payload to verify final payload bytes)");
    } else {
      console.log("VERIFY OK (manifest signature valid; unsigned payload)");
    }
    return;
  }

  usage();
  process.exit(2);
}

import { pathToFileURL } from "node:url";

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  main();
}
