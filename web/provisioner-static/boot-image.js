//
// boot-image.js — builds the "scripts" partition's LittleFS image from an
// authored boot.fnl (B3 of the Fennel scripting UI plan: "bake the
// authored show into the flash image"). Wraps web/vendor/littlefs-image.wasm
// (the real upstream littlefs C library compiled freestanding to wasm32 --
// see web/littlefs-image/shim.c and scripts/vendor_littlefs_image_wasm.sh).
//
// Pure computation, zero imports -- unlike wasmoon this needs no glue
// module, just plain WebAssembly.instantiate.
//

let modulePromise = null;

async function getModule() {
  if (modulePromise) return modulePromise;
  modulePromise = (async () => {
    const resp = await fetch(new URL("./vendor/littlefs-image.wasm", import.meta.url));
    const bytes = await resp.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(bytes, {});
    return instance.exports;
  })();
  return modulePromise;
}

// Builds the scripts-partition image (a fixed-size Uint8Array matching
// firmware/partitions.csv's "scripts" partition) containing a single
// boot.fnl file with `bootFnlSource` as its content. Throws if the source
// doesn't fit the wasm module's fixed buffers or the underlying littlefs
// build fails (see web/littlefs-image/shim.c's lfs_image_build contract).
export async function buildScriptsImage(bootFnlSource) {
  const m = await getModule();
  const nameBytes = new TextEncoder().encode("boot.fnl");
  const contentBytes = new TextEncoder().encode(bootFnlSource);

  if (nameBytes.length >= m.lfs_image_name_buf_cap()) {
    throw new Error("internal error: 'boot.fnl' doesn't fit the name buffer");
  }
  if (contentBytes.length > m.lfs_image_content_buf_cap()) {
    throw new Error(
      `boot.fnl is ${contentBytes.length} bytes, which is larger than this tool supports ` +
      `(${m.lfs_image_content_buf_cap()} bytes). Split it into a smaller boot.fnl that ` +
      `loads the rest via glow.save from the live console instead.`,
    );
  }

  // Re-read `memory.buffer` after each wasm call that could grow it
  // (a `Uint8Array` view over a detached/resized ArrayBuffer throws).
  new Uint8Array(m.memory.buffer).set(nameBytes, m.lfs_image_name_buf_ptr());
  new Uint8Array(m.memory.buffer).set(contentBytes, m.lfs_image_content_buf_ptr());

  const rc = m.lfs_image_build(nameBytes.length, contentBytes.length);
  if (rc !== 0) {
    throw new Error(`littlefs image build failed (lfs error ${rc})`);
  }

  const diskPtr = m.lfs_image_disk_ptr();
  const diskSize = m.lfs_image_disk_size();
  // Slice (copy), not a view: the caller holds onto this after further
  // wasm calls (or module reuse) could invalidate the underlying buffer.
  return new Uint8Array(m.memory.buffer.slice(diskPtr, diskPtr + diskSize));
}
