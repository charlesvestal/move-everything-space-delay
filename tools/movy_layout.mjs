/* movy_layout.mjs — boot the REAL Movy model with this module in an FX slot
 * and print the pages exactly as they render on the Move.
 *
 * Movy resolves a module's layout from
 *   /data/UserData/schwung/modules/<category>/<id>/movy_config.json
 * where an fxN component maps to `audio_fx` (movy src/modules/loader.ts), so
 * this serves our config at that path and boots componentKey `fx1`.
 *
 * Usage:  node tools/movy_layout.mjs <path-to-movy-checkout> [--json]
 * The checkout must have `npm install && npm run build:browser` run in it.
 */

import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const MOVY = process.argv[2];
const AS_JSON = process.argv.includes('--json');
if (!MOVY) {
    console.error('usage: node tools/movy_layout.mjs <movy-checkout> [--json]');
    process.exit(2);
}

const moduleJson = JSON.parse(readFileSync(join(ROOT, 'src/module.json'), 'utf8'));
const movyConfig = readFileSync(join(ROOT, 'src/movy_config.json'), 'utf8');
const chainParams = moduleJson.capabilities.chain_params;
const ID = moduleJson.id;
const CK = 'fx1';

const { installEnv } = await import(join(MOVY, 'browser-test', 'env.mjs'));
const env = installEnv();
globalThis.os = {
    readdir: () => [[], 0],
    stat: () => [{ mode: 0x8000, size: 0 }, 0],
};
const CONFIG_PATH =
    `/data/UserData/schwung/modules/audio_fx/${ID}/movy_config.json`;
let configRead = false;
globalThis.host_read_file = (path) => {
    if (path === CONFIG_PATH) { configRead = true; return movyConfig; }
    return null;
};

/* Seed the param store the way the chain host serves an FX slot. */
const params = {};
for (const p of chainParams) {
    const fallback = p.type === 'enum' ? (p.options?.[0] ?? '0') : (p.min ?? 0);
    params[`${CK}:${p.key}`] = String(p.default ?? fallback);
}
params[`${CK}:chain_params`] = JSON.stringify(chainParams);
params[`${CK}:name`] = moduleJson.name;
params[`${CK}_module`] = ID;
env.setParams(params);

const { createModel } = await import(join(MOVY, 'dist', 'esm', 'model', 'index.js'));
const model = createModel(0, CK);
model.reload();
/* Movy refreshes ONE param per tick (store.ts refreshOneParam: even ticks walk
 * the current page, odd ticks walk all slots round-robin), so a short settle
 * leaves the last slots reading "..." purely because the cursor has not reached
 * them yet. Tick well past 2 x the slot count so every value is real. */
for (let i = 0; i < 400; i++) model.tick();

const pages = [];
const pageCount = model.getBankCount();
for (let pg = 0; pg < pageCount; pg++) {
    const vm = model.getViewModel();
    pages.push({
        name: vm.bankName,
        rows: vm.rows.map(row => row.map(p => p && {
            shortName: p.shortName,
            fullName: p.fullName,
            type: p.type,
            renderStyle: p.renderStyle,
            displayValue: p.displayValue,
            automatable: p.automatable,
        })),
    });
    model.changePage(1);
}

const result = { id: ID, configRead, pageCount, pages };

if (AS_JSON) {
    console.log(JSON.stringify(result, null, 1));
} else {
    console.log(`module   : ${ID} (component ${CK} -> audio_fx)`);
    console.log(`config   : ${configRead ? 'READ from ' + CONFIG_PATH
                                         : '*** NOT READ — movy fell back ***'}`);
    console.log(`pages    : ${pageCount}`);
    for (const pg of pages) {
        console.log(`\n── ${pg.name} `.padEnd(64, '─'));
        pg.rows.forEach((row, ri) => {
            const cells = row.map(p => p
                ? `${(p.shortName ?? '').padEnd(5)} ${String(p.displayValue ?? '').padEnd(8)}`
                : '·'.padEnd(14));
            console.log(`  row${ri}: ` + cells.join('| '));
            console.log('         ' + row.map(p => p
                ? ` ${(p.fullName ?? '').slice(0, 13).padEnd(13)}`
                : ' '.repeat(14)).join(' '));
        });
    }
}

/* Gate: every chain param the module publishes should be reachable on a page
 * (meters and hidden compatibility params excepted). */
const onPage = new Set();
for (const pg of pages)
    for (const row of pg.rows)
        for (const p of row)
            if (p) onPage.add(p.fullName);
const missing = chainParams
    .filter(p => !onPage.has(p.name))
    .map(p => p.key);

if (!configRead) {
    console.error('\nFAIL: movy did not read our movy_config.json');
    process.exit(1);
}
if (missing.length) {
    console.error(`\nFAIL: params not on any movy page: ${missing.join(', ')}`);
    process.exit(1);
}
console.log(`\nOK: config read, ${pageCount} pages, all ${chainParams.length} params reachable`);
