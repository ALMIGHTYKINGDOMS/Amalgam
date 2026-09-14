// Quick brace/paren/bracket balance checker for C++ files.
// Char-literal and string aware (handles '\'', "..." including escapes).
import fs from "node:fs";
for (const file of process.argv.slice(2)) {
  const text = fs.readFileSync(file, "utf-8");
  const stack = [];
  const pairs = { ")": "(", "]": "[", "}": "{" };
  let line = 1;
  let inString = null;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === "\n") { line++; continue; }
    if (inString) {
      if (inString === "//") { if (c === "\n") inString = null; continue; }
      if (inString === "/*") { if (c === "*" && text[i + 1] === "/") { inString = null; i++; } continue; }
      if (c === "\\") { i++; continue; }
      if (c === inString) inString = null;
      continue;
    }
    if (c === "/" && text[i + 1] === "/") { inString = "//"; i++; continue; }
    if (c === "/" && text[i + 1] === "*") { inString = "/*"; i++; continue; }
    if (c === '"' || c === "'") { inString = c; continue; }
    if (c === "(" || c === "[" || c === "{") stack.push({ c, line });
    else if (c === ")" || c === "]" || c === "}") {
      const top = stack.pop();
      if (!top || top.c !== pairs[c]) {
        console.log(`${file}:${line}: unbalanced ${c} (expected ${pairs[c]})`);
        process.exit(1);
      }
    }
  }
  if (inString === "//" || inString === "/*") { /* unterminated comment */ }
  if (stack.length) {
    const tail = stack.slice(-10);
    console.log(`${file}: unclosed ${stack.length} token(s); last opens:`);
    for (const s of tail) console.log(`  ${s.c} opened at line ${s.line}`);
    process.exit(1);
  }
  console.log(`${file}: balanced`);
}
