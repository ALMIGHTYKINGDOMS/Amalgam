const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const pkg = readJson("package.json");
const errors = [];
const required = [
  "behavior_pack/manifest.json",
  "behavior_pack/scripts/main.js",
  "behavior_pack/scripts/connection.js",
  "behavior_pack/scripts/bootstrap.js",
  "behavior_pack/scripts/menu.js",
  "behavior_pack/scripts/hud.js",
  "behavior_pack/scripts/settings.js",
  "behavior_pack/scripts/diagnostics.js",
  "resource_pack/manifest.json",
  "resource_pack/texts/en_US.lang"
];

function readJson(relative) {
  const file = path.join(root, relative);
  try { return JSON.parse(fs.readFileSync(file, "utf8")); }
  catch (error) { errors.push(`${relative}: invalid JSON (${error.message})`); return {}; }
}

for (const relative of required) {
  if (!fs.existsSync(path.join(root, relative))) errors.push(`${relative}: required file is missing`);
}

const behavior = readJson("behavior_pack/manifest.json");
const resource = readJson("resource_pack/manifest.json");
const manifests = [behavior, resource];
const uuids = [];
const uuidPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;

for (const [name, manifest] of [["behavior", behavior], ["resource", resource]]) {
  const headerUuid = manifest.header?.uuid;
  if (!uuidPattern.test(headerUuid ?? "")) errors.push(`${name}: header UUID is invalid`);
  if (!Array.isArray(manifest.header?.version) || manifest.header.version.length !== 3) errors.push(`${name}: header version is invalid`);
  uuids.push(headerUuid);
  for (const module of manifest.modules ?? []) {
    if (!uuidPattern.test(module.uuid ?? "")) errors.push(`${name}: module UUID is invalid`);
    uuids.push(module.uuid);
  }
}

if (new Set(uuids).size !== uuids.length) errors.push("manifests: duplicate UUID detected");
if (behavior.dependencies?.filter((dep) => dep.uuid).length !== 1) errors.push("behavior: resource-pack dependency is missing or duplicated");
if (behavior.dependencies?.filter((dep) => dep.module_name === "@minecraft/server").length !== 1) errors.push("behavior: Script API dependency is missing");
if (behavior.dependencies?.filter((dep) => dep.module_name === "@minecraft/server-ui").length !== 1) errors.push("behavior: UI API dependency is missing");
if (behavior.dependencies?.[0]?.uuid !== resource.header?.uuid) errors.push("behavior: first dependency must be the Amalgam resource pack");

const expectedVersion = pkg.version?.split("-")[0].split(".").map(Number).join(".");
for (const manifest of manifests) {
  if ((manifest.header?.version ?? []).join(".") !== expectedVersion) errors.push("package: manifest version does not match package.json");
}

const secretPatterns = [
  /postgres(?:ql)?:\/\//i,
  /service[_-]?role/i,
  /sb_secret_/i,
  /-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----/i,
  /(?:whsec|turn_secret|client_secret)\s*[:=]/i,
  /(?:api[_-]?key|token)\s*[:=]\s*["'](?:sk|pk|sb|cf)[A-Za-z0-9_.\-/]+/i
];
const skip = new Set(["node_modules", "dist", "build"]);
function scan(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (skip.has(entry.name)) continue;
    const full = path.join(dir, entry.name);
    // This validator contains the patterns it is checking for; it is a build
    // tool and is not copied into the .mcaddon package.
    if (full === __filename) continue;
    if (entry.isDirectory()) scan(full);
    else if (entry.isFile() && /\.(js|json|md|txt|lang)$/.test(entry.name)) {
      const text = fs.readFileSync(full, "utf8");
      if (secretPatterns.some((pattern) => pattern.test(text))) errors.push(`${path.relative(root, full)}: possible secret detected`);
    }
  }
}
scan(root);

if (errors.length) {
  console.error("Bedrock validation failed:");
  for (const error of errors) console.error(`- ${error}`);
  process.exit(1);
}

console.log(`Bedrock validation passed: ${pkg.name} ${pkg.version}`);
console.log(`Behavior UUID: ${behavior.header.uuid}`);
console.log(`Resource UUID: ${resource.header.uuid}`);
console.log(`Minimum Bedrock: ${pkg.amalgam.minimum_bedrock}`);
console.log(`Scripts: ${required.filter((file) => file.endsWith(".js")).length} required entry points present`);
