import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";

await mkdir("build/v02", { recursive: true });
execFileSync("g++", [
  "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
  "src/rvi-v02.cpp", "tests/rvi-v02.test.cpp", "-o", "build/v02/v02-verify"
], { stdio: "inherit" });

const inputs = await Promise.all([
  readFile("tests/fixtures/v02-scenes.txt"),
  readFile("src/rvi-v02.cpp"),
  readFile("tests/rvi-v02.test.cpp"),
]);
const digest = data => createHash("sha256").update(data).digest("hex");
let stdout;
try {
  stdout = execFileSync("build/v02/v02-verify", ["tests/fixtures/v02-scenes.txt", "build/v02/hits.csv"],
    { encoding: "utf8", stdio: ["ignore", "pipe", "inherit"] });
} catch (error) {
  stdout = error.stdout?.toString("utf8") ?? "";
  if (!stdout) throw error;
}
const report = JSON.parse(stdout);
const hits = await readFile("build/v02/hits.csv");
const hitLines = hits.toString("utf8").split("\n").length - 2;
report.output_digests = { hits: digest(hits) };
report.input_digests = {
  scene: digest(inputs[0]),
  renderer: digest(inputs[1]),
  oracle: digest(inputs[2]),
};
await writeFile("build/v02/report.json", JSON.stringify(report, null, 2) + "\n");
assert.equal(report.case_count, 40);
assert.equal(hitLines, report.samples * 4);
assert.equal(report.mismatches, 0);
assert.equal(report.mutations_detected, report.mutations_total);
assert.ok(report.samples >= 43560);
assert.equal(report.ok, true);
console.log("V02: " + report.case_count + " scenarios/views, " + report.samples +
  " samples, " + report.mutations_detected + "/" + report.mutations_total + " negative tests PASS");
