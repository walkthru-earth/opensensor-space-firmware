// Writes js_numbers.txt, the golden table for opensensor::json::format_real.
// Each line is the IEEE 754 bits of a double as 16 hex digits, a space and
// String(value) from the JavaScript engine that ran this script.
//
// Run with Node from the repo root.
//   node tools/fixtures/json/generate_js_numbers.mjs tools/fixtures/stac > tools/fixtures/json/js_numbers.txt
// The vendored STAC fixtures supply the rows and example Items, so the table
// covers every number those hold and every value widened from float32, plus
// edge cases and seeded random bit patterns.
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join } from "node:path";

const root = process.argv[2];
if (!root) throw new Error("usage: generate_js_numbers.mjs <fixture dir>");
const values = new Map();
const view = new DataView(new ArrayBuffer(8));
const bits = (v) => {
  view.setFloat64(0, v);
  return view.getBigUint64(0).toString(16).padStart(16, "0");
};
const add = (v) => {
  if (typeof v !== "number" || !Number.isFinite(v)) return;
  values.set(bits(v), v);
  // Math.fround of a large double is Infinity, which JSON cannot hold.
  const single = Math.fround(v);
  if (Number.isFinite(single)) values.set(bits(single), single);
};
const walk = (node) => {
  if (Array.isArray(node)) node.forEach(walk);
  else if (node && typeof node === "object") Object.values(node).forEach(walk);
  else add(node);
};
const files = (dir) =>
  readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    return statSync(path).isDirectory() ? files(path) : [path];
  });
for (const path of files(root))
  if (path.endsWith(".json")) walk(JSON.parse(readFileSync(path, "utf8")));

const edges = [
  0, -0, 1, -1, 0.1, 0.2, 0.3, 1 / 3, 2 / 3, 100, 1e6, 1e7, 123456789,
  1e20, 1e21, 1.5e21, 123e19, 999999999999999999999, 1e-6, 1e-7, 1.5e-7,
  0.000001, 0.0000012345, 5e-324, 2.2250738585072014e-308,
  1.7976931348623157e308, 9007199254740991, 9007199254740993,
  4.35, 0.5, 2.675, 1.005, 0.07, 1e23, 8.41e21, 5e-7, 123.456,
  -180, 180, -90, 90, 31.2345678, 29.968826711496935, 31.00000000000001,
];
edges.forEach(add);
for (let e = -330; e <= 310; ++e) {
  add(Number(`1e${e}`));
  add(Number(`4.9e${e}`));
  add(-Number(`7.25e${e}`));
}
// xorshift64 with a fixed seed, so the table is reproducible.
let state = 0x9e3779b97f4a7c15n;
const mask = (1n << 64n) - 1n;
for (let i = 0; i < 4000; ++i) {
  state ^= (state << 13n) & mask;
  state ^= state >> 7n;
  state ^= (state << 17n) & mask;
  view.setBigUint64(0, state);
  add(view.getFloat64(0));
}
const lines = [...values.entries()]
  .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))
  .map(([b, v]) => `${b} ${String(v)}`);
process.stdout.write(lines.join("\n") + "\n");
