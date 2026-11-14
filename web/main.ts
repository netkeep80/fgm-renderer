import createScene from "./fgm_scene.mjs";

const canvas = document.querySelector<HTMLCanvasElement>("canvas")!;
const status = document.querySelector<HTMLElement>("[role=status]")!;
const reset = document.querySelector<HTMLButtonElement>("#reset")!;
const context = canvas.getContext("2d");
const workgroupSize = 64;

async function resource(name: string) {
  const response = await fetch(new URL(name, import.meta.url));
  if (!response.ok) throw new Error(`Не удалось загрузить ${name} (${response.status})`);
  return response;
}

async function loadScene() {
  const [module, source, shader] = await Promise.all([
    createScene(),
    resource("sphere.fgm.json").then(response => response.arrayBuffer()),
    resource("scene.wgsl").then(response => response.text()),
  ]);
  const size = module._fgm_scene_size();
  const input = module._malloc(source.byteLength);
  let output = 0;
  try {
    output = module._malloc(size * size * 4);
    if (!input || !output) throw new Error("Недостаточно памяти для сцены");
    module.HEAPU8.set(new Uint8Array(source), input);
    if (module._fgm_sample_scene(input, source.byteLength, output) !== 0) {
      throw new Error("Ядро FGM отклонило пакет сцены");
    }
    return { size, shader, field: module.HEAPU8.slice(output, output + size * size * 4) };
  } finally {
    module._free(input);
    module._free(output);
  }
}

type Scene = Awaited<ReturnType<typeof loadScene>>;

async function render(device: GPUDevice, scene: Scene) {
  const bytes = scene.field.byteLength;
  const input = device.createBuffer({ size: bytes, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  const output = device.createBuffer({ size: bytes, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
  const readback = device.createBuffer({ size: bytes, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
  try {
    const pipeline = await device.createComputePipelineAsync({
      layout: "auto",
      compute: { module: device.createShaderModule({ code: scene.shader }), entryPoint: "main" },
    });
    const bindings = device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [{ binding: 0, resource: { buffer: input } }, { binding: 1, resource: { buffer: output } }],
    });
    device.queue.writeBuffer(input, 0, scene.field);
    const command = device.createCommandEncoder();
    const pass = command.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, bindings);
    pass.dispatchWorkgroups(Math.ceil(scene.size * scene.size / workgroupSize));
    pass.end();
    command.copyBufferToBuffer(output, 0, readback, 0, bytes);
    device.queue.submit([command.finish()]);
    await readback.mapAsync(GPUMapMode.READ);
    return new Uint8ClampedArray(readback.getMappedRange().slice(0));
  } finally {
    input.destroy();
    output.destroy();
    readback.destroy();
  }
}

let active: GPUDevice | undefined;
let frame = 0;

function show(message: string, state: string) {
  status.textContent = message;
  status.dataset.state = state;
}

async function start(scene: Scene, recovering = false) {
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) throw new Error("WebGPU-адаптер недоступен");
  const device = await adapter.requestDevice();
  active = device;
  void device.lost.then(async info => {
    if (active !== device) return;
    active = undefined;
    reset.disabled = true;
    if (recovering && info.reason !== "destroyed") {
      show("WebGPU-устройство снова потеряно. Перезагрузите страницу.", "error");
      return;
    }
    show("Восстанавливаем WebGPU-устройство…", "recovering");
    try { await start(scene, info.reason !== "destroyed"); }
    catch (error) { fail(error); }
  });
  try {
    const pixels = await render(device, scene);
    if (active !== device) return;
    if (!context) throw new Error("Холст недоступен");
    canvas.width = scene.size;
    canvas.height = scene.size;
    context.putImageData(new ImageData(pixels, scene.size, scene.size), 0, 0);
    canvas.dataset.frame = String(++frame);
    recovering = false;
    show("Кадр готов", "ready");
    reset.disabled = false;
  } catch (error) {
    // Потеря устройства может быть объявлена после ошибки команды.
    if (active === device) {
      reset.disabled = false;
      fail(error);
    }
  }
}

function fail(error: unknown) {
  show(error instanceof Error ? error.message : String(error), "error");
}

reset.addEventListener("click", () => {
  if (!active) return;
  active.destroy();
  reset.disabled = true;
  show("Пересоздаём WebGPU-устройство…", "recovering");
});

const scene = navigator.gpu ? loadScene() : undefined;
if (scene) {
  scene.then(value => start(value)).catch(fail);
} else {
  show("WebGPU недоступен в этом браузере или контексте", "unsupported");
}
