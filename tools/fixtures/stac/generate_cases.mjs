// Writes cases.json, the extra golden cases for stac-item-test (#142).
// Each case is one of the vendored fixture files with a few edits, and the
// result canonical.mjs buildItem gives for it, an Item or a no Item reason.
// The file also holds round2, rfc3339 and Item id results from the same
// script, so the board helpers are checked against the reference itself.
//
// Run with Node from the repo root, against a stac-opensensor checkout at the
// pinned commit with its npm dependencies installed (h3-js 4.5.0).
//   node tools/fixtures/stac/generate_cases.mjs <stac-opensensor>/scripts/canonical.mjs tools/fixtures/stac > tools/fixtures/stac/cases.json
// It refuses any canonical.mjs other than the one at commit 83787768.
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const [canonicalPath, root] = process.argv.slice(2);
if (!canonicalPath || !root)
  throw new Error("usage: generate_cases.mjs <canonical.mjs> <fixture dir>");
const CANONICAL_SHA256 =
  "98583cdb3c5b625f0d998e4650ee3a303c2ab4addd53f9486820c641182b9090";
const actual = createHash("sha256").update(readFileSync(canonicalPath)).digest("hex");
if (actual !== CANONICAL_SHA256)
  throw new Error(`canonical.mjs is ${actual}, not the 83787768 bytes`);
const canonical = await import(pathToFileURL(resolve(canonicalPath)).href);
const h3 = await import(
  pathToFileURL(join(resolve(canonicalPath), "../../node_modules/h3-js/dist/h3-js.es.js")).href
).catch(() => null);
if (!h3) throw new Error("h3-js is not installed next to canonical.mjs");
const h3Version = JSON.parse(
  readFileSync(join(resolve(canonicalPath), "../../node_modules/h3-js/package.json"), "utf8"),
).version;
if (h3Version !== "4.5.0") throw new Error(`h3-js ${h3Version}, not 4.5.0`);

const read = (path) => JSON.parse(readFileSync(join(root, path), "utf8"));
const BASES = {
  cores3: {
    rows: "fixtures/thing=3f2b9c1e-6a7d-4e58-9b1c-2d4f6a8e0c13/data_1300_0f3a9c2e5b7d41e8a6c3f0b2d9e4a713_2881-2970-0.rows.json",
    template: "templates/m5stack-cores3/3b4d8500a19f42c6743b0f7d725276c96ca14a0c249f613048603afbc3cfaff9.json",
  },
  waveshare: {
    rows: "fixtures/thing=7c1e4b9a-3d52-4f86-a0b7-e9d2c5f81a46/data_0815_5e9d0c7b31a84f62b8d47a10c3e6f295_412-487-0.rows.json",
    template: "templates/waveshare-esp32-s3-sim7670g/a8605aafb6f76b794e9e7fb32927be6d203afeeb14527206c0e49c1eb0436809.json",
  },
};
const load = (base) => {
  const fixture = read(BASES[base].rows);
  return {
    file: { ...fixture.file },
    rows: structuredClone(fixture.rows),
    footer: structuredClone(read(fixture.footer)),
    template: read(BASES[base].template),
  };
};

// The edits stac-item-test applies the same way. A row list of "all" means
// every row.
const apply = (input, ops) => {
  for (const op of ops) {
    if (op.op === "set") {
      const values = input.rows[op.column];
      const rows = op.rows === "all" ? values.map((_, i) => i) : op.rows;
      for (const i of rows) values[i] = op.value;
    } else if (op.op === "file") {
      input.file[op.key] = op.value;
    } else if (op.op === "thing") {
      input.footer.thing[op.key] = op.value;
    } else if (op.op === "truncate") {
      for (const name of Object.keys(input.rows))
        input.rows[name] = input.rows[name].slice(0, op.rows);
    } else {
      throw new Error(`unknown op ${op.op}`);
    }
  }
  return input;
};

const home = "887576970bfffff"; // the cores3 fixture cell
const ring = h3.gridDisk(home, 1).filter((c) => c !== home).sort();
const far = h3.latLngToCell(-33.8688, 151.2093, 8); // Sydney, far from Cairo
const child = h3.cellToChildren(home, 9)[3];
const parent = h3.cellToParent(home, 7);
const rowsOf = (base) => load(base).rows.event_time_utc_us.length;
const n = { cores3: rowsOf("cores3"), waveshare: rowsOf("waveshare") };
const spread = (cells, count) => [...Array(count).keys()].map((i) => cells[i % cells.length]);
const setEach = (column, values) =>
  values.map((value, i) => ({ op: "set", column, rows: [i], value }));

const CASES = [
  ["cores3-resolution-9", "cores3", [{ op: "set", column: "h3_cell_id", rows: [7], value: child }]],
  ["cores3-resolution-7", "cores3", [{ op: "set", column: "h3_cell_id", rows: "all", value: parent }]],
  ["cores3-invalid-cell", "cores3", [{ op: "set", column: "h3_cell_id", rows: [0], value: "8875769700fffff" }]],
  ["waveshare-resolution-9", "waveshare", [{ op: "set", column: "h3_cell_id", rows: [20], value: child }]],
  ["cores3-null-time", "cores3", [{ op: "set", column: "event_time_utc_us", rows: [5], value: null }]],
  ["cores3-no-cell", "cores3", [{ op: "set", column: "h3_cell_id", rows: "all", value: null }]],
  ["cores3-zero-cell", "cores3", [{ op: "set", column: "h3_cell_id", rows: "all", value: "000000000000000" }]],
  ["cores3-one-located-row", "cores3", [
    { op: "set", column: "h3_cell_id", rows: "all", value: "000000000000000" },
    { op: "set", column: "h3_cell_id", rows: [44], value: home },
  ]],
  ["cores3-two-cells", "cores3", [{ op: "set", column: "h3_cell_id", rows: [10], value: ring[0] }]],
  ["cores3-bad-name", "cores3", [{ op: "file", key: "name", value: "data_1300_0F3A9C2E5B7D41E8A6C3F0B2D9E4A713_2881-2970-0.parquet" }]],
  ["cores3-benchmark-name", "cores3", [{ op: "file", key: "name", value: "bench_1300_0f3a9c2e5b7d41e8a6c3f0b2d9e4a713_2881-2970-0.parquet" }]],
  ["cores3-no-thing", "cores3", [{ op: "thing", key: "id", value: "" }]],
  ["cores3-no-rows", "cores3", [{ op: "truncate", rows: 0 }]],
  ["cores3-one-row", "cores3", [{ op: "truncate", rows: 1 }]],
  // Every gate passes, so every reading counts.
  ["cores3-gates-pass", "cores3", [
    { op: "set", column: "pms_status", rows: "all", value: 4 },
    { op: "set", column: "light_status", rows: "all", value: 3 },
  ]],
  // A null gate value fails the gate.
  ["cores3-gate-null", "cores3", [{ op: "set", column: "pms_status", rows: "all", value: null }]],
  ["cores3-gates-fail", "cores3", [
    { op: "set", column: "pms_status", rows: "all", value: 2 },
    { op: "set", column: "light_status", rows: "all", value: 2 },
    { op: "set", column: "proximity_status", rows: "all", value: 1 },
  ]],
  ["cores3-negative-readings", "cores3", [
    ...setEach("ambient_temperature_c", spread([-1.005, -2.675, -0.125, -40.5, 0.004999, -0.005], n.cores3)),
  ]],
  ["cores3-fraction-times", "cores3", [
    { op: "set", column: "event_time_utc_us", rows: [0], value: 1792843200123400 },
    { op: "set", column: "event_time_utc_us", rows: [89], value: 1792844099999999 },
  ]],
  ["cores3-moving", "cores3", [{ op: "thing", key: "mobility", value: "moving" }]],
  // Six cells around a missing centre make one polygon with a hole.
  ["waveshare-ring-hole", "waveshare", [...setEach("h3_cell_id", spread(ring, n.waveshare))]],
  ["waveshare-far-apart", "waveshare", [
    { op: "set", column: "h3_cell_id", rows: "all", value: home },
    { op: "set", column: "h3_cell_id", rows: [30, 31], value: far },
  ]],
  // Unsorted runs, A B A, so the board must sort and drop repeats.
  ["waveshare-revisit", "waveshare", [
    ...setEach("h3_cell_id", spread([ring[4], ring[1], ring[4], home, ring[1]], n.waveshare)),
  ]],
  ["waveshare-fixed", "waveshare", [
    { op: "thing", key: "mobility", value: "fixed" },
    { op: "set", column: "h3_cell_id", rows: "all", value: home },
  ]],
];

const cases = CASES.map(([name, base, ops]) => {
  const input = apply(load(base), ops);
  const result = canonical.buildItem(input);
  return { name, base, ops, expect: "none" in result ? { none: result.none } : { item: result.item } };
});

// round2 over edges, fixture style means and the percentages a file of up
// to 720 rows can hold. Doubles travel as IEEE 754 bit patterns.
const view = new DataView(new ArrayBuffer(8));
const bits = (v) => {
  view.setFloat64(0, v);
  return view.getBigUint64(0).toString(16).padStart(16, "0");
};
let state = 0x2545f4914f6cdd1dn;
const mask = (1n << 64n) - 1n;
const next = () => {
  state ^= (state << 13n) & mask;
  state ^= state >> 7n;
  state ^= (state << 17n) & mask;
  return state;
};
const round2Inputs = [
  0, -0, 1, -1, 0.5, 1.005, 2.675, 4.35, -1.005, -2.675, 0.125, 0.135, -0.135, 0.005, -0.005,
  0.004999, 99.995, -99.995, 123456.785, 1e15 + 0.5, 1e-10, -1e-10, 5e-324, 1 / 3, 2 / 3,
  100, 33.333333333333336, 66.66666666666667, 12.345, 1e300, -1e300,
];
for (let i = 0; i < 2000; ++i) {
  const r = Number(next() % 720n) + 1;
  const c = Number(next() % BigInt(r + 1));
  round2Inputs.push((100 * c) / r);
  const scale = [1, 10, 100, 1000, 10000][Number(next() % 5n)];
  const value = (Number(next() % 2000000n) - 1000000) / scale;
  round2Inputs.push(value / (Number(next() % 719n) + 1));
}
const round2 = round2Inputs.map((v) => [bits(v), bits(canonical.round2(v))]);

const LIMIT = 4102444800000000;
const rfcInputs = [
  0, 1, 10, 999999, 1000000, 1100000, 1234560, 59999999, 86399999999, 86400000000,
  951782400000000, 951868799999999, Date.UTC(2096, 1, 29) * 1000 + 999999, LIMIT - 1,
  1792843200000000, 1792843200123400, 1792844099999999,
];
for (let i = 0; i < 1000; ++i) rfcInputs.push(Number(next() % BigInt(LIMIT)));
const rfc3339 = rfcInputs.map((us) => [us, canonical.rfc3339(us)]);

const hex32 = "0f3a9c2e5b7d41e8a6c3f0b2d9e4a713";
const idNames = [
  `data_1300_${hex32}_2881-2970-0.parquet`,
  `data_0000_${hex32}_0-0-0.parquet`,
  `data_9999_${hex32}_18446744073709551615-1-123.parquet`,
  `data_1300_${hex32.toUpperCase()}_2881-2970-0.parquet`,
  `data_1300_${hex32.slice(1)}_2881-2970-0.parquet`,
  `data_1300_${hex32}0_2881-2970-0.parquet`,
  `data_130_${hex32}_2881-2970-0.parquet`,
  `data_13000_${hex32}_2881-2970-0.parquet`,
  `data_1300_${hex32}_2881-2970.parquet`,
  `data_1300_${hex32}_2881-2970-0-1.parquet`,
  `data_1300_${hex32}_-2970-0.parquet`,
  `data_1300_${hex32}_2881--0.parquet`,
  `data_1300_${hex32}_2881-2970-.parquet`,
  `data_1300_${hex32}_2881-2970-0a.parquet`,
  `data_1300_${hex32}_2881-2970-0.parquet.partial`,
  `data_1300_${hex32}_2881-2970-0.json`,
  `data_1300_${hex32}_2881-2970-0.parquet\n`,
  `xdata_1300_${hex32}_2881-2970-0.parquet`,
  `bench_1300_${hex32}_2881-2970-0.parquet`,
  `data_13a0_${hex32}_2881-2970-0.parquet`,
  `data-1300_${hex32}_2881-2970-0.parquet`,
  `data_1300-${hex32}_2881-2970-0.parquet`,
  `data_1300_${hex32}-2881-2970-0.parquet`,
  `data_1300_${hex32}_2881_2970_0.parquet`,
  `data_1300_${hex32}_+2881-2970-0.parquet`,
  `data_1300_${hex32.slice(0, 31)}g_2881-2970-0.parquet`,
  `data_1300_${hex32}_2881-2970-0.Parquet`,
  `data_١300_${hex32}_2881-2970-0.parquet`,
  ".parquet",
  "",
  "data_.parquet",
];
// The name pattern of canonical.mjs buildItem, copied as is.
const ITEM_ID = /^(data_[0-9]{4}_[0-9a-f]{32}_[0-9]+-[0-9]+-[0-9]+)\.parquet$/;
const itemIds = idNames.map((name) => [name, ITEM_ID.exec(name)?.[1] ?? null]);

// One entry per line, so a regenerated file diffs by case.
const list = (entries) => `[\n${entries.map((e) => JSON.stringify(e)).join(",\n")}\n]`;
process.stdout.write(`{
"source": ${JSON.stringify("stac-opensensor 83787768cc8451109d48a03d4ef5493ffcd403dd scripts/canonical.mjs")},
"canonical_sha256": ${JSON.stringify(CANONICAL_SHA256)},
"h3_js": ${JSON.stringify(h3Version)},
"cases": ${list(cases)},
"round2": ${list(round2)},
"rfc3339": ${list(rfc3339)},
"item_ids": ${list(itemIds)}
}
`);
