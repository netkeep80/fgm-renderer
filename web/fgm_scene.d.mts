interface SceneModule {
  HEAPU8: Uint8Array;
  _malloc(size: number): number;
  _free(pointer: number): void;
  _fgm_scene_size(): number;
  _fgm_sample_scene(input: number, length: number, output: number): number;
}
export default function createScene(): Promise<SceneModule>;
