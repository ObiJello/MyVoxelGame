// tools/protocol-gen/generate_block_states.js
import fs from "node:fs";
import path from "node:path";
import semver from "node:semver"; // Node 22 has built-in semver? If not, `npm i semver` and: import semver from "semver";
import minecraftData from "minecraft-data";

// IMPORTANT: import the version catalog JSON directly from the package
// Node ESM supports JSON imports with an assertion.
import pcVersions from "minecraft-data/data/pc/common/versions.json" assert { type: "json" };

const requested = process.argv[2] || "1.21.4";

// Build the list of published Java releases we actually have in this installed package
const releases = (pcVersions?.release ?? []).map(v => v.minecraftVersion);

// Helper: pick highest shipped version <= requested, else latest shipped
function resolveVersion(req) {
    if (!releases || releases.length === 0) return null;
    // semver requires proper X.Y.Z, mc tags are already in that shape
    const pick =
        semver.maxSatisfying(releases, "<=" + req) || releases[releases.length - 1];
    return pick || null;
}

const chosen = resolveVersion(requested);
if (!chosen) {
    console.error("Could not resolve a Java release from minecraft-data’s versions.json.");
    process.exit(1);
}
if (chosen !== requested) {
    console.warn(`[protocol-gen] Requested ${requested} not found; using ${chosen}.`);
}

let mcData;
try {
    mcData = minecraftData(chosen);
} catch (e) {
    console.error(`minecraft-data('${chosen}') threw:`, e?.message || e);
    process.exit(1);
}

if (!mcData || !mcData.blockStates || mcData.blockStates.length === 0) {
    // Help debug what’s actually in the dataset so you can see keys if this ever fails
    console.error(`Dataset for ${chosen} has no blockStates. Available keys:`,
        mcData ? Object.keys(mcData).sort().join(", ") : "<none>");
    process.exit(1);
}

// Build output table: index by global state id
const out = {
    version: chosen,
    generatedAt: new Date().toISOString(),
    count: mcData.blockStates.length,
    states: []
};

for (const s of mcData.blockStates) {
    out.states[s.id] = {
        name: s.name,                 // e.g. "minecraft:oak_stairs"
        properties: s.properties || {}// e.g. { facing:"east", half:"top", waterlogged:"false", ... }
    };
}

const outDir = path.resolve(process.cwd(), "../../assets/protocol", chosen);
fs.mkdirSync(outDir, { recursive: true });
const jsonPath = path.join(outDir, "block_state_table.json");
fs.writeFileSync(jsonPath, JSON.stringify(out, null, 2));
console.log(`✔ Wrote ${jsonPath} (states=${out.count})`);