@group(0) @binding(0) var<storage, read> field: array<f32>;
@group(0) @binding(1) var<storage, read_write> pixels: array<u32>;

fn rgba(r: u32, g: u32, b: u32) -> u32 {
  return r | (g << 8u) | (b << 16u) | 0xff000000u;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) invocation: vec3<u32>) {
  let i = invocation.x;
  if (i >= arrayLength(&field)) { return; }
  let value = field[i];
  if (value >= 0.0) {
    pixels[i] = rgba(15u, 22u, 34u);
  } else {
    let band = min(7u, u32(-value * 8.0));
    pixels[i] = rgba(67u + band * 18u, 169u + band * 10u, 151u + band * 7u);
  }
}
