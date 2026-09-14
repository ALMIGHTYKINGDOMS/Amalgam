import test from "node:test";
import assert from "node:assert/strict";
import { validateConfig } from "./config.js";

test("validateConfig accepts a pre-provisioned HTTPS node", () => {
  assert.deepEqual(validateConfig({
    supabaseUrl: "https://example.supabase.co",
    supabaseKey: "anon",
    supabaseAccessToken: "",
    nodeId: "node-id",
    nodeSecret: "a".repeat(32),
  }), []);
});

test("validateConfig rejects insecure and unscoped startup", () => {
  const errors = validateConfig({
    supabaseUrl: "http://example.invalid",
    supabaseKey: "anon",
    supabaseAccessToken: "",
    nodeId: "",
    nodeSecret: "secret",
  });
  assert.match(errors.join(" | "), /AMALGAM_NODE_ID/);
  assert.match(errors.join(" | "), /HTTPS/);
});
