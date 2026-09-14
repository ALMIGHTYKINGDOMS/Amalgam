const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const os = require("node:os");
const { spawnSync } = require("node:child_process");

const root = path.resolve(__dirname, "..");
const pkg = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
const buildRoot = path.join(root, "build");
const distRoot = path.join(root, "dist");
const stagingRoot = path.join(buildRoot, "staging");
const version = pkg.version;
const archiveName = `AmalgamBedrockClient-${version}.mcaddon`;

function tarPath(value) {
  // Git for Windows' GNU tar treats a raw `C:\\...` archive path as a
  // remote host specification. Normalize absolute Windows paths to the
  // POSIX form understood by the same tar binary in every shell context.
  if (typeof value !== "string") return value;
  const match = value.match(/^([A-Za-z]):[\\/](.*)$/);
  return match ? `/${match[1].toLowerCase()}/${match[2].replace(/[\\\\]+/g, "/")}` : value;
}

function runTar(args, options = {}) {
  const normalizedArgs = args.map(tarPath);
  const result = spawnSync("tar.exe", normalizedArgs, { windowsHide: true, ...options });
  if (result.error || result.status !== 0) {
    const detail = result.stderr ? `: ${String(result.stderr).trim()}` : "";
    throw result.error || new Error(`tar.exe exited with ${result.status}${detail}`);
  }
  return result;
}

function remove(target) { fs.rmSync(target, { recursive: true, force: true }); }
function copy(source, target) { fs.cpSync(source, target, { recursive: true }); }

const validation = spawnSync(process.execPath, [path.join(__dirname, "validate.js")], { stdio: "inherit" });
if (validation.error) throw validation.error;
if (validation.status !== 0) process.exit(validation.status ?? 1);
remove(stagingRoot);
fs.mkdirSync(stagingRoot, { recursive: true });
remove(distRoot);
fs.mkdirSync(distRoot, { recursive: true });

const behaviorStage = path.join(stagingRoot, "behavior");
const resourceStage = path.join(stagingRoot, "resource");
copy(path.join(root, "behavior_pack"), behaviorStage);
copy(path.join(root, "resource_pack"), resourceStage);

// Reuse the approved Amalgam logo as the pack icon. The launcher owns this
// source asset; the Bedrock package receives a copy and no external URL.
const logo = path.resolve(root, "..", "..", "docs", "branding", "amalgam-logo.png");
if (fs.existsSync(logo)) {
  copy(logo, path.join(behaviorStage, "pack_icon.png"));
  copy(logo, path.join(resourceStage, "pack_icon.png"));
  fs.mkdirSync(path.join(resourceStage, "textures", "ui"), { recursive: true });
  copy(logo, path.join(resourceStage, "textures", "ui", "amalgam_logo.png"));
  const art = [
    ["client-menu-header-ai.png", "amalgam_client_menu_header.png"],
    ["client-hud-icon-atlas-ai.png", "amalgam_hud_icon_atlas.png"],
    ["client-diagnostics-ai.png", "amalgam_diagnostics.png"]
  ];
  for (const [sourceName, targetName] of art) {
    const source = path.resolve(root, "..", "..", "docs", "branding", "ai", sourceName);
    if (fs.existsSync(source)) copy(source, path.join(resourceStage, "textures", "ui", targetName));
  }
} else {
  throw new Error("docs/branding/amalgam-logo.png is missing");
}

const behaviorMcpack = path.join(stagingRoot, "Amalgam Bedrock Behavior Pack.mcpack");
const resourceMcpack = path.join(stagingRoot, "Amalgam Bedrock Resource Pack.mcpack");
runTar(["-a", "-c", "-f", behaviorMcpack, "-C", behaviorStage, "."]);
runTar(["-a", "-c", "-f", resourceMcpack, "-C", resourceStage, "."]);

const output = path.join(distRoot, archiveName);
runTar(["-a", "-c", "-f", output, "-C", stagingRoot, path.basename(behaviorMcpack), path.basename(resourceMcpack)]);
const stableOutput = path.join(distRoot, "AmalgamBedrockClient.mcaddon");
fs.copyFileSync(output, stableOutput);

// Verify the final archive, not only the source tree. This catches malformed
// outer packages before the launcher or an installer ever receives them.
const archiveListing = runTar(["-t", "-f", output], { encoding: "utf8" }).stdout || "";
for (const expected of [path.basename(behaviorMcpack), path.basename(resourceMcpack)]) {
  if (!archiveListing.split(/\r?\n/).some((entry) => entry.trim() === expected)) {
    throw new Error(`Final Bedrock archive is missing ${expected}`);
  }
}
const archiveCheckRoot = fs.mkdtempSync(path.join(os.tmpdir(), "amalgam-bedrock-package-"));
try {
  runTar(["-x", "-f", output, "-C", archiveCheckRoot]);
  for (const packName of [path.basename(behaviorMcpack), path.basename(resourceMcpack)]) {
    const packPath = path.join(archiveCheckRoot, packName);
    const packListing = runTar(["-t", "-f", packPath], { encoding: "utf8" }).stdout || "";
    if (!packListing.split(/\r?\n/).some((entry) => entry.trim().replace(/\\/g, "/").endsWith("/manifest.json") || entry.trim() === "manifest.json")) {
      throw new Error(`Nested Bedrock pack is missing manifest.json: ${packName}`);
    }
  }
} finally {
  fs.rmSync(archiveCheckRoot, { recursive: true, force: true });
}

const hash = crypto.createHash("sha256").update(fs.readFileSync(output)).digest("hex");
const inventory = {
  name: "Amalgam Bedrock Client",
  package: archiveName,
  version,
  channel: pkg.amalgam.channel,
  minimum_bedrock: pkg.amalgam.minimum_bedrock,
  script_api: pkg.amalgam.script_api,
  server_ui_api: pkg.amalgam.server_ui_api,
  created_at: new Date().toISOString(),
  sha256: hash,
  contents: [path.basename(behaviorMcpack), path.basename(resourceMcpack)]
};
fs.writeFileSync(path.join(distRoot, "package-inventory.json"), `${JSON.stringify(inventory, null, 2)}\n`, "utf8");
fs.writeFileSync(path.join(distRoot, "package-sha256.txt"), `${hash}  ${archiveName}\n`, "utf8");
console.log(`Created ${path.relative(root, output)}`);
console.log(`Stable package ${path.relative(root, stableOutput)}`);
console.log(`SHA-256 ${hash}`);
