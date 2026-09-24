const fs = require("node:fs");
const path = require("node:path");
const { numericVersionParts, renderRuntimeMetadata } = require("./runtime-metadata.js");

const root = path.resolve(__dirname, "..");
const packagePath = path.join(root, "package.json");
const pkg = JSON.parse(fs.readFileSync(packagePath, "utf8"));
const version = process.argv[2];

if (!version) {
  console.log(`${pkg.name} ${pkg.version} (${pkg.amalgam.channel})`);
  console.log(`Minimum Bedrock: ${pkg.amalgam.minimum_bedrock}`);
  process.exit(0);
}

if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(version)) {
  console.error("Version must be semantic, for example 3.0.0-beta.2");
  process.exit(1);
}

pkg.version = version;
const versionParts = numericVersionParts(version, "Version");
const minimumBedrockParts = numericVersionParts(pkg.amalgam.minimum_bedrock, "Minimum Bedrock version");
for (const manifestName of ["behavior_pack/manifest.json", "resource_pack/manifest.json"]) {
  const manifestPath = path.join(root, manifestName);
  const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
  manifest.header.version = versionParts;
  manifest.header.min_engine_version = minimumBedrockParts;
  for (const module of manifest.modules ?? []) module.version = versionParts;
  if (manifestName === "behavior_pack/manifest.json") {
    for (const dependency of manifest.dependencies ?? []) {
      if (dependency.uuid) dependency.version = versionParts;
      if (dependency.module_name === "@minecraft/server") dependency.version = pkg.amalgam.script_api;
      if (dependency.module_name === "@minecraft/server-ui") dependency.version = pkg.amalgam.server_ui_api;
    }
  }
  fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
}
fs.writeFileSync(packagePath, `${JSON.stringify(pkg, null, 2)}\n`, "utf8");
fs.writeFileSync(
  path.join(root, "behavior_pack", "scripts", "util", "runtime_metadata.js"),
  renderRuntimeMetadata(pkg),
  "utf8"
);
console.log(`Updated Bedrock client to ${version} (${pkg.amalgam.channel})`);
