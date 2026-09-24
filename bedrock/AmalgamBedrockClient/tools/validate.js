const fs = require("node:fs");
const path = require("node:path");
const { numericVersionParts, renderRuntimeMetadata } = require("./runtime-metadata.js");

const root = path.resolve(__dirname, "..");
const errors = [];
const pkg = readJson("package.json");
const required = [
  "behavior_pack/manifest.json",
  "behavior_pack/scripts/main.js",
  "behavior_pack/scripts/connection.js",
  "behavior_pack/scripts/bootstrap.js",
  "behavior_pack/scripts/menu.js",
  "behavior_pack/scripts/hud.js",
  "behavior_pack/scripts/settings.js",
  "behavior_pack/scripts/diagnostics.js",
  "behavior_pack/scripts/util/runtime_metadata.js",
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

let expectedVersionParts = [];
let expectedMinimumBedrockParts = [];
try {
  expectedVersionParts = numericVersionParts(pkg.version, "package version");
  expectedMinimumBedrockParts = numericVersionParts(pkg.amalgam?.minimum_bedrock, "minimum Bedrock version");
} catch (error) {
  errors.push(`package: ${error.message}`);
}
const expectedVersion = expectedVersionParts.join(".");
const expectedMinimumBedrock = expectedMinimumBedrockParts.join(".");
for (const [name, manifest] of [["behavior", behavior], ["resource", resource]]) {
  if ((manifest.header?.version ?? []).join(".") !== expectedVersion) errors.push(`${name}: header version does not match package.json`);
  if ((manifest.header?.min_engine_version ?? []).join(".") !== expectedMinimumBedrock) errors.push(`${name}: minimum Bedrock version does not match package.json`);
  for (const module of manifest.modules ?? []) {
    if ((module.version ?? []).join(".") !== expectedVersion) errors.push(`${name}: module version does not match package.json`);
  }
}

const resourceDependency = behavior.dependencies?.find((dependency) => dependency.uuid);
const scriptApiDependency = behavior.dependencies?.find((dependency) => dependency.module_name === "@minecraft/server");
const serverUiDependency = behavior.dependencies?.find((dependency) => dependency.module_name === "@minecraft/server-ui");
if ((resourceDependency?.version ?? []).join(".") !== expectedVersion) errors.push("behavior: resource-pack dependency version does not match package.json");
if (scriptApiDependency?.version !== pkg.amalgam?.script_api) errors.push("behavior: Script API version does not match package.json");
if (serverUiDependency?.version !== pkg.amalgam?.server_ui_api) errors.push("behavior: UI API version does not match package.json");

try {
  const runtimeMetadataPath = path.join(root, "behavior_pack", "scripts", "util", "runtime_metadata.js");
  const actualRuntimeMetadata = fs.readFileSync(runtimeMetadataPath, "utf8").replace(/\r\n/g, "\n");
  const expectedRuntimeMetadata = renderRuntimeMetadata(pkg);
  if (actualRuntimeMetadata !== expectedRuntimeMetadata) errors.push("runtime metadata does not match package.json; run npm run version -- <version>");
} catch (error) {
  errors.push(`runtime metadata: ${error.message}`);
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
