import { copyFile, mkdir } from "node:fs/promises";

await mkdir("build/web", { recursive: true });
for (const name of ["index.html", "sphere.fgm.json", "scene.wgsl"]) {
  await copyFile(`web/${name}`, `build/web/${name}`);
}
