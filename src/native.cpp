#include "scene.hpp"

#include <webgpu/wgpu.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
template<auto Release, class Handle>
auto own(Handle handle) {
  if (!handle) throw std::runtime_error("Не удалось создать ресурс WebGPU");
  return std::unique_ptr<std::remove_pointer_t<Handle>, decltype(Release)>(handle, Release);
}

WGPUStringView view(std::string_view text) {
  return {text.data(), text.size()};
}

std::string read(const char* path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error(std::string("Не удалось открыть ") + path);
  return {std::istreambuf_iterator<char>(input), {}};
}

template<class State>
void await(WGPUInstance instance, const State& state, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!state.done) {
    wgpuInstanceProcessEvents(instance);
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
    if (!state.done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

template<class Handle>
struct Request {
  Handle handle{};
  bool done = false;
};

struct MapState { bool done = false; bool success = false; };

std::vector<std::uint8_t> render(const std::vector<float>& field, const std::string& shader) {
  Request<WGPUAdapter> adapter_request;
  Request<WGPUDevice> device_request;
  MapState mapped;
  auto instance = own<wgpuInstanceRelease>(wgpuCreateInstance(nullptr));
  auto adapter_callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  adapter_callback.mode = WGPUCallbackMode_AllowProcessEvents;
  adapter_callback.userdata1 = &adapter_request;
  adapter_callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter,
                                 WGPUStringView, void* data, void*) {
    auto& request = *static_cast<Request<WGPUAdapter>*>(data);
    if (status == WGPURequestAdapterStatus_Success) request.handle = adapter;
    request.done = true;
  };
  auto adapter_options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
  wgpuInstanceRequestAdapter(instance.get(), &adapter_options, adapter_callback);
  await(instance.get(), adapter_request, "WebGPU: истекло время поиска адаптера");
  if (!adapter_request.handle) throw std::runtime_error("WebGPU-адаптер недоступен");
  auto adapter = own<wgpuAdapterRelease>(adapter_request.handle);

  auto device_callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
  device_callback.mode = WGPUCallbackMode_AllowProcessEvents;
  device_callback.userdata1 = &device_request;
  device_callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice device,
                                WGPUStringView, void* data, void*) {
    auto& request = *static_cast<Request<WGPUDevice>*>(data);
    if (status == WGPURequestDeviceStatus_Success) request.handle = device;
    request.done = true;
  };
  wgpuAdapterRequestDevice(adapter.get(), nullptr, device_callback);
  await(instance.get(), device_request, "WebGPU: истекло время создания устройства");
  if (!device_request.handle) throw std::runtime_error("WebGPU-устройство недоступно");
  auto device = own<wgpuDeviceRelease>(device_request.handle);
  auto queue = own<wgpuQueueRelease>(wgpuDeviceGetQueue(device.get()));

  const auto bytes = field.size() * sizeof(float);
  auto buffer = [&](WGPUBufferUsage usage) {
    auto desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = bytes;
    desc.usage = usage;
    return own<wgpuBufferRelease>(wgpuDeviceCreateBuffer(device.get(), &desc));
  };
  auto input = buffer(WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  auto output = buffer(WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
  auto readback = buffer(WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);

  auto wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = view(shader);
  auto shader_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  shader_desc.nextInChain = &wgsl.chain;
  auto module = own<wgpuShaderModuleRelease>(wgpuDeviceCreateShaderModule(device.get(), &shader_desc));
  auto pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
  pipeline_desc.compute.module = module.get();
  pipeline_desc.compute.entryPoint = view("main");
  auto pipeline = own<wgpuComputePipelineRelease>(
      wgpuDeviceCreateComputePipeline(device.get(), &pipeline_desc));

  auto layout = own<wgpuBindGroupLayoutRelease>(wgpuComputePipelineGetBindGroupLayout(pipeline.get(), 0));
  WGPUBindGroupEntry entries[] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
  entries[0].buffer = input.get();
  entries[0].size = bytes;
  entries[1].binding = 1;
  entries[1].buffer = output.get();
  entries[1].size = bytes;
  auto group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  group_desc.layout = layout.get();
  group_desc.entryCount = 2;
  group_desc.entries = entries;
  auto group = own<wgpuBindGroupRelease>(wgpuDeviceCreateBindGroup(device.get(), &group_desc));

  wgpuQueueWriteBuffer(queue.get(), input.get(), 0, field.data(), bytes);
  auto encoder = own<wgpuCommandEncoderRelease>(wgpuDeviceCreateCommandEncoder(device.get(), nullptr));
  auto pass = own<wgpuComputePassEncoderRelease>(wgpuCommandEncoderBeginComputePass(encoder.get(), nullptr));
  wgpuComputePassEncoderSetPipeline(pass.get(), pipeline.get());
  wgpuComputePassEncoderSetBindGroup(pass.get(), 0, group.get(), 0, nullptr);
  wgpuComputePassEncoderDispatchWorkgroups(pass.get(), static_cast<std::uint32_t>((field.size() + 63) / 64), 1, 1);
  wgpuComputePassEncoderEnd(pass.get());
  wgpuCommandEncoderCopyBufferToBuffer(encoder.get(), output.get(), 0, readback.get(), 0, bytes);
  auto commands = own<wgpuCommandBufferRelease>(wgpuCommandEncoderFinish(encoder.get(), nullptr));
  auto command = commands.get();
  wgpuQueueSubmit(queue.get(), 1, &command);

  auto map_callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  map_callback.mode = WGPUCallbackMode_AllowProcessEvents;
  map_callback.userdata1 = &mapped;
  map_callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* data, void*) {
    auto& state = *static_cast<MapState*>(data);
    state.success = status == WGPUMapAsyncStatus_Success;
    state.done = true;
  };
  wgpuBufferMapAsync(readback.get(), WGPUMapMode_Read, 0, bytes, map_callback);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!mapped.done && std::chrono::steady_clock::now() < deadline) {
    wgpuDevicePoll(device.get(), false, nullptr);
    wgpuInstanceProcessEvents(instance.get());
    if (!mapped.done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!mapped.success) {
    wgpuBufferUnmap(readback.get());
    wgpuInstanceProcessEvents(instance.get());
    throw std::runtime_error("WebGPU: не удалось прочитать кадр");
  }
  const auto* pixels = static_cast<const std::uint8_t*>(
      wgpuBufferGetConstMappedRange(readback.get(), 0, bytes));
  if (!pixels) throw std::runtime_error("WebGPU: пустой буфер кадра");
  std::vector<std::uint8_t> copy(pixels, pixels + bytes);
  wgpuBufferUnmap(readback.get());
  return copy;
}

void write_ppm(const char* path, const std::vector<std::uint8_t>& rgba) {
  std::ofstream file(path, std::ios::binary);
  file << "P6\n" << scene_size << ' ' << scene_size << "\n255\n";
  for (std::size_t i = 0; i < rgba.size(); i += 4) {
    file.write(reinterpret_cast<const char*>(rgba.data() + i), 3);
  }
  file.close();
  if (!file) throw std::runtime_error("Не удалось сохранить кадр");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Использование: fgm_boot <сцена.json> <шейдер.wgsl> <кадр.ppm>\n";
    return 2;
  }
  try {
    std::vector<float> field(scene_size * scene_size);
    sample_scene(read(argv[1]), std::span<float, scene_size * scene_size>(field));
    write_ppm(argv[3], render(field, read(argv[2])));
    std::cout << "Кадр сохранён: " << argv[3] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
