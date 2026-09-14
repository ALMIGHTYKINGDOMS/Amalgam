// Bounded, rotating console-log persistence for managed servers.
//
// Each complete console line is appended to a JSONL file so that server
// output survives agent restarts (and is available for diagnostics without
// a running process). Files are rotated by size and pruned by count so the
// log can never grow without bound.
//
// Format: one JSON object per line:
//   { "n": line_number, "s": source, "l": level, "c": content, "t": timestamp }
//
// Line numbers are persisted so recovery can continue numbering across
// restarts and across rotation boundaries.

import fs from "node:fs";
import path from "node:path";

const DEFAULT_OPTIONS = Object.freeze({
  maxBytes: 5 * 1024 * 1024, // 5 MB per file
  maxFiles: 4, // active file + .1 .. .3
  restoreLines: 500, // tail restored at startup
});

/**
 * Append one console entry to the rolling log, rotating when the active
 * file exceeds maxBytes. Failures are best-effort: console persistence must
 * never take down a server operation.
 *
 * @param {string} logPath - path of the active log file
 * @param {object} [options]
 * @param {object} entry - { line_number, source, level, content, timestamp }
 */
export function appendConsoleLine(logPath, options = {}, entry) {
  const opts = { ...DEFAULT_OPTIONS, ...options };
  try {
    const line = JSON.stringify({
      n: entry.line_number,
      s: entry.source,
      l: entry.level,
      c: entry.content,
      t: entry.timestamp || new Date().toISOString(),
    }) + "\n";
    fs.mkdirSync(path.dirname(logPath), { recursive: true });
    fs.appendFileSync(logPath, line, "utf-8");
    const st = fs.statSync(logPath);
    if (st.size >= opts.maxBytes) rotateConsoleLog(logPath, opts.maxFiles);
  } catch (err) {
    // Best-effort only
    console.error(`[console-log] Append failed: ${err.message}`);
  }
}

/**
 * Rotate the active log file: console.log -> console.log.1 -> ... and start
 * a fresh active file. Older files are deleted once maxFiles is exceeded.
 */
export function rotateConsoleLog(logPath, maxFiles = DEFAULT_OPTIONS.maxFiles) {
  try {
    for (let i = maxFiles - 1; i >= 1; i--) {
      const from = i === 1 ? logPath : `${logPath}.${i - 1}`;
      const to = `${logPath}.${i}`;
      if (fs.existsSync(from)) {
        fs.rmSync(to, { force: true });
        fs.renameSync(from, to);
      }
    }
    fs.writeFileSync(logPath, "", "utf-8");
  } catch (err) {
    console.error(`[console-log] Rotation failed: ${err.message}`);
  }
}

/**
 * Recover the tail of a server's console from the persisted log (active
 * file plus rotation files, oldest last). Returns the most recent
 * `restoreLines` entries ordered by line_number, plus the last line number
 * so numbering continues across restarts.
 *
 * @returns {{ lines: object[], lastLineNumber: number }}
 */
export function recoverConsoleLog(logPath, options = {}) {
  const opts = { ...DEFAULT_OPTIONS, ...options };
  const records = [];
  for (let i = opts.maxFiles - 1; i >= 1; i--) {
    readLogFile(`${logPath}.${i}`, records);
  }
  readLogFile(logPath, records);
  records.sort((a, b) => a.line_number - b.line_number);
  const lines = records.slice(-opts.restoreLines);
  return {
    lines,
    lastLineNumber: lines.length ? lines[lines.length - 1].line_number : 0,
  };
}

function readLogFile(filePath, records) {
  try {
    if (!fs.existsSync(filePath)) return;
    const raw = fs.readFileSync(filePath, "utf-8");
    for (const line of raw.split("\n")) {
      if (!line.trim()) continue;
      try {
        const obj = JSON.parse(line);
        if (obj && typeof obj.n === "number" && typeof obj.c === "string") {
          records.push({
            line_number: obj.n,
            source: obj.s === "system" ? "system" : "server",
            level: obj.l || "info",
            content: obj.c,
            raw: obj.c,
            timestamp: obj.t || new Date().toISOString(),
          });
        }
      } catch {
        // Skip malformed lines; recovery must be tolerant of truncation
      }
    }
  } catch (err) {
    console.error(`[console-log] Recovery failed for ${filePath}: ${err.message}`);
  }
}
