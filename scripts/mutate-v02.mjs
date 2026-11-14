// Actual source-level mutation testing of the CPU DUT. Every mutant is
// compiled into an executable and must be killed by the regular oracle suite.
// Compiler failures are never counted as kills.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";

const original = await readFile("src/rvi-v02.cpp", "utf8");
const oracle = "tests/rvi-v02.test.cpp";
const dir = "build/v02/mutants";
await mkdir(dir, { recursive: true });
const mutation = (name, originalText, replacement) => ({ name, originalText, replacement });
const mutations = [
  mutation("cube_entry_instead_of_surface",
    "Hit{true,t,b.id,material}", "Hit{true,enter,b.id,material}"),
  mutation("closed_sphere_boundary",
    "std::hypot(q.x,q.y,q.z) - b.radius < 0",
    "std::hypot(q.x,q.y,q.z) - b.radius <= 0"),
  mutation("c02_wrong_boundary_owner",
    "if (value < -kDomain+i*step) return i-1;",
    "if (value <= -kDomain+i*step) return i-1;"),
  mutation("ignore_single_owner",
    "if (!ignore_owner && !owns(ray,t,divisions,ix,iy,iz)) continue;",
    "if (ignore_owner && !owns(ray,t,divisions,ix,iy,iz)) continue;"),
  mutation("material_sort_instead_of_depth",
    "right.depth<left.depth || (right.depth==left.depth && right.fragment<left.fragment)",
    "right.material<left.material || (right.material==left.material && right.fragment<left.fragment)"),
  mutation("reverse_equal_depth_tie",
    "right.fragment<left.fragment", "right.fragment>left.fragment"),
  mutation("child_average_before_composition",
    "result.coverage += 1.0/static_cast<double>(offsets.size());",
    "result.coverage += 0.5/static_cast<double>(offsets.size());"),
  mutation("child_local_raster_phase",
    "const Ray ray=global_ray(camera,{x,y,offset[0],offset[1]});",
    "const Ray ray=global_ray(camera,{x,y,1.0-offset[0],offset[1]});"),
  mutation("last_nonempty_wins_instead_of_min",
    "right.depth<left.depth || (right.depth==left.depth && right.fragment<left.fragment)",
    "true"),
  mutation("allow_conflicting_material_for_same_key",
    "left.material!=right.material)", "false)"),
  mutation("keep_negative_zero",
    "if (hit.depth==0) hit.depth=0.0;", "if (hit.depth==0) hit.depth=-0.0;"),
  mutation("discard_panel_hole",
    "return std::max(outer,-opening) < 0;", "return std::min(outer,-opening) < 0;"),
];
const rows = [];
for (const { name, originalText, replacement } of mutations) {
  assert.equal(original.split(originalText).length - 1, 1,
    "У мутации " + name + " должна быть ровно одна цель");
  const source = original.replace(originalText, replacement);
  const filename = join(dir, name + ".cpp");
  const binary = join(dir, name);
  await writeFile(filename, source);
  const build = spawnSync("g++", [
    "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
    "-Isrc", filename, oracle, "-o", binary,
  ], { encoding: "utf8", maxBuffer: 1024 * 1024 * 8 });
  if (build.error || build.status !== 0) {
    throw new Error("Не удалось собрать мутацию " + name + ": " + (build.stderr ?? build.error));
  }
  const execution = spawnSync(binary,
    ["tests/fixtures/v02-scenes.txt", join(dir, name + ".csv")],
    { encoding: "utf8", maxBuffer: 1024 * 1024 * 8 });
  const contractRejected = !execution.error && execution.status === 3 &&
    /E_FRAGMENT_CONFLICT|E_NONFINITE_DEPTH|E_FRAGMENT_IDENTITY/.test(execution.stderr ?? "");
  const mismatchRejected = !execution.error && execution.status === 2;
  const killed = mismatchRejected || contractRejected;
  rows.push({ mutation: name, compiled: true, exit_status: execution.status,
    killed_by_behavioral_tests: mismatchRejected,
    rejected_by_contract: contractRejected,
    detected: killed,
    stderr_excerpt: killed ? "" : (execution.stderr ?? "").slice(-1000),
    source_sha256: createHash("sha256").update(source).digest("hex") });
  console.log("V02 mutation " + name + ": " + (killed ? "KILLED" : "SURVIVED/ERROR") +
    " (exit " + execution.status + (contractRejected ? ", invariant rejection" : "") + ")");
  if (!killed) console.error((execution.stderr ?? "").slice(-1000));
}
const killed = rows.filter(x => x.detected).length;
const report = { schema:"fgm-v02-source-mutation-report-1", total:rows.length,
  compiled:rows.length, killed, survived:rows.length-killed, results:rows };
await writeFile("build/v02/mutations.json", JSON.stringify(report, null, 2) + "\n");
assert.equal(killed,rows.length,"Не все исполняемые мутации отвергнуты");
console.log("V02: " + killed + "/" + rows.length + " real mutated DUT binaries killed");
