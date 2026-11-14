import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { createServer } from "node:http";
import { test } from "node:test";
import { chromium } from "playwright";

const contentTypes = new Map([
  ["/", ["index.html", "text/html; charset=utf-8"]],
  ["/main.js", ["main.js", "text/javascript"]],
  ["/fgm_scene.mjs", ["fgm_scene.mjs", "text/javascript"]],
  ["/fgm_scene.wasm", ["fgm_scene.wasm", "application/wasm"]],
  ["/sphere.fgm.json", ["sphere.fgm.json", "application/json"]],
  ["/scene.wgsl", ["scene.wgsl", "text/plain"]],
]);
const web = new URL("../build/web/", import.meta.url);

async function runBrowser(action) {
  const server = createServer(async (request, response) => {
    const asset = contentTypes.get(request.url);
    if (!asset) {
      response.writeHead(404).end();
      return;
    }
    try {
      const bytes = await readFile(new URL(asset[0], web));
      response.writeHead(200, { "Content-Type": asset[1] }).end(bytes);
    } catch {
      response.writeHead(500).end();
    }
  });
  await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
  try {
    const browser = await chromium.launch({
      executablePath: process.env.FGM_CHROMIUM,
      channel: process.env.FGM_CHROMIUM ? undefined : "chromium",
      args: ["--enable-unsafe-webgpu", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
    });
    try {
      await action(browser, `http://127.0.0.1:${server.address().port}/`);
    } finally {
      await browser.close();
    }
  } finally {
    await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  }
}

const pixels = page => page.locator("canvas").evaluate(canvas =>
  [...canvas.getContext("2d").getImageData(0, 0, canvas.width, canvas.height).data]);

async function ready(page, frame) {
  await page.waitForFunction(expected => {
    const state = document.querySelector("[role=status]").dataset.state;
    return ["error", "unsupported"].includes(state) ||
      (state === "ready" && document.querySelector("canvas").dataset.frame === expected);
  }, String(frame), { timeout: 25000 });
  const status = page.getByRole("status");
  assert.equal(await status.getAttribute("data-state"), "ready", await status.innerText());
}

test("кадр WASM и WebGPU совпадает с нативным, устройство восстанавливается", { timeout: 70000 }, async () => {
  const native = await readFile(process.env.FGM_NATIVE_FRAME ?? "build/native/sphere.ppm");
  const header = Buffer.from("P6\n128 128\n255\n");
  assert.deepEqual(native.subarray(0, header.length), header);
  const rgb = native.subarray(header.length);
  assert.equal(rgb.length, 128 * 128 * 3);
  await runBrowser(async (browser, url) => {
    const page = await browser.newPage();
    const errors = [];
    page.on("pageerror", error => errors.push(error.message));
    await page.goto(url);
    await ready(page, 1);
    const first = await pixels(page);
    assert.deepEqual(first.slice(0, 4), [15, 22, 34, 255]);
    assert.notDeepEqual(first.slice(4 * (64 * 128 + 64), 4 * (64 * 128 + 64) + 4), first.slice(0, 4));
    const browserRgb = Buffer.alloc(rgb.length);
    for (let i = 0; i < first.length; i += 4) {
      assert.equal(first[i + 3], 255);
      browserRgb.set(first.slice(i, i + 3), i / 4 * 3);
    }
    assert.deepEqual(browserRgb, rgb);
    await page.getByRole("button").click();
    await ready(page, 2);
    assert.deepEqual(await pixels(page), first);
    assert.deepEqual(errors, []);
    if (process.env.FGM_BROWSER_SCREENSHOT) await page.screenshot({ path: process.env.FGM_BROWSER_SCREENSHOT });
  });
});

test("страница объясняет отсутствие WebGPU", { timeout: 35000 }, async () => {
  await runBrowser(async (browser, url) => {
    const page = await browser.newPage();
    await page.addInitScript(() => Object.defineProperty(navigator, "gpu", { value: undefined }));
    await page.goto(url);
    const status = page.getByRole("status");
    assert.equal(await status.getAttribute("data-state"), "unsupported");
    assert.match(await status.innerText(), /WebGPU недоступен/);
  });
});
