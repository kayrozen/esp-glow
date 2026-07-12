#!/usr/bin/env node
// run_wasm_dump.mjs — vendoring-time-only helper (see dump_fixed_image.c):
// runs the *wasm32* build with the identical fixed test vector, in plain
// Node (no browser needed -- the module is pure computation with zero
// imports), and writes the raw disk bytes to argv[3].
import fs from "fs";

const [, , wasmPath, outPath] = process.argv;
if (!wasmPath || !outPath) {
  console.error("usage: run_wasm_dump.mjs <wasm-path> <output-path>");
  process.exit(2);
}

const bytes = fs.readFileSync(wasmPath);
const { instance } = await WebAssembly.instantiate(bytes, {});
const m = instance.exports;
const mem = new Uint8Array(m.memory.buffer);

const name = new TextEncoder().encode("boot.fnl");
const content = new TextEncoder().encode(
  "(fn breathe [t]\n" +
  "  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2))))))\n" +
  "(glow.cue.define :breathe {:effects [breathe]})\n" +
  "(glow.cue.go :breathe)\n"
);

mem.set(name, m.lfs_image_name_buf_ptr());
mem.set(content, m.lfs_image_content_buf_ptr());

const rc = m.lfs_image_build(name.length, content.length);
if (rc !== 0) {
  console.error("lfs_image_build failed:", rc);
  process.exit(1);
}

const diskPtr = m.lfs_image_disk_ptr();
const diskSize = m.lfs_image_disk_size();
const disk = Buffer.from(m.memory.buffer, diskPtr, diskSize);
fs.writeFileSync(outPath, disk);
