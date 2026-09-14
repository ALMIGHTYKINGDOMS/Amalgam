import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  appendConsoleLine,
  rotateConsoleLog,
  recoverConsoleLog,
} from "./console-log.js";

function tempDir() {
  return fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-console-log-"));
}

test("append + recover round-trips entries with line numbers", () => {
  const dir = tempDir();
  const logPath = path.join(dir, "console.jsonl");
  const entries = [
    { line_number: 1, source: "server", level: "info", content: "Starting minecraft server version 1.21.4" },
    { line_number: 2, source: "system", level: "error", content: "Exception in thread \"main\"" },
    { line_number: 3, source: "server", level: "warn", content: "deprecated option" },
  ];
  for (const e of entries) appendConsoleLine(logPath, {}, e);

  const { lines, lastLineNumber } = recoverConsoleLog(logPath);
  assert.equal(lastLineNumber, 3);
  assert.equal(lines.length, 3);
  assert.equal(lines[0].line_number, 1);
  assert.equal(lines[0].content, "Starting minecraft server version 1.21.4");
  assert.equal(lines[1].source, "system");
  assert.equal(lines[1].level, "error");
  assert.equal(lines[2].level, "warn");
  fs.rmSync(dir, { recursive: true, force: true });
});

test("rotation bounds the number of files and keeps old content recoverable", () => {
  const dir = tempDir();
  const logPath = path.join(dir, "console.jsonl");
  // Force rotation with a tiny maxBytes so appends trigger several rotations
  const opts = { maxBytes: 80, maxFiles: 3 };
  let lineNumber = 0;
  for (let i = 0; i < 30; i++) {
    appendConsoleLine(logPath, opts, {
      line_number: ++lineNumber,
      source: "server",
      level: "info",
      content: `line number ${i} with some padding to exceed the size cap`,
    });
  }
  const files = fs.readdirSync(dir).sort();
  // Active file + at most maxFiles - 1 rotation files
  assert.ok(files.length <= 3, `expected <= 3 log files, got ${files.length}`);
  assert.ok(files.every((f) => f.startsWith("console.jsonl")));

  const { lines, lastLineNumber } = recoverConsoleLog(logPath, opts);
  assert.equal(lastLineNumber, 30);
  assert.ok(lines.length > 0, "recovery should return content after rotation");
  // Lines must be returned in order and be a contiguous tail of the log
  for (let i = 1; i < lines.length; i++) {
    assert.ok(lines[i].line_number > lines[i - 1].line_number);
  }
  assert.equal(lines[lines.length - 1].content, "line number 29 with some padding to exceed the size cap");
  fs.rmSync(dir, { recursive: true, force: true });
});

test("recovery tolerates truncated or malformed lines", () => {
  const dir = tempDir();
  const logPath = path.join(dir, "console.jsonl");
  fs.writeFileSync(
    logPath,
    '{"n":1,"s":"server","l":"info","c":"good line","t":"2026-01-01T00:00:00Z"}\n' +
      '{"n":2,"s":"server","l":"info","c":"truncated at end' +
      "NOT JSON AT ALL\n" +
      '{"n":3,"s":"server","l":"info","c":"after the junk","t":"2026-01-01T00:00:00Z"}\n',
    "utf-8"
  );
  const { lines, lastLineNumber } = recoverConsoleLog(logPath);
  assert.equal(lastLineNumber, 3);
  assert.deepEqual(lines.map((l) => l.content), ["good line", "after the junk"]);
  fs.rmSync(dir, { recursive: true, force: true });
});

test("rotateConsoleLog is idempotent on a fresh log", () => {
  const dir = tempDir();
  const logPath = path.join(dir, "console.jsonl");
  rotateConsoleLog(logPath, 3);
  assert.ok(fs.existsSync(logPath));
  fs.rmSync(dir, { recursive: true, force: true });
});
