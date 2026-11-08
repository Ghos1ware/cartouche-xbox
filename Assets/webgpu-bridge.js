// Cartouche for Xbox: WebGPU in the game's page, drawn by the app (native Dawn on DirectX 12).
// Every WebGPU call is recorded as a command (Bridge/PROTOCOL.md) and replayed by the app; the picture is the
// app's, under this page (its background transparent). The page keeps its JavaScript, DOM, sound, controller.
(() => {
  "use strict";
  const host = window.chrome && window.chrome.webview;
  if (!host || window.__cartoucheGpuBridge) return;
  const realGpu = navigator.gpu;

  // ---------------------------------------------------------------- transport
  const shared = []; // { u8, busy }
  let pending = []; // Uint8Array chunks waiting for a free shared buffer
  let ready = null; // the app's adapter, once said
  let unavailable = null;
  const readyWaiters = [];
  const callbacks = new Map();
  let nextCallback = 1;
  let nextId = 1;
  const devices = new Map();

  class Writer {
    constructor() { this.reset(); }
    reset() { this.buf = new ArrayBuffer(1 << 16); this.u8 = new Uint8Array(this.buf); this.dv = new DataView(this.buf); this.at = 0; }
    room(n) {
      if (this.at + n <= this.buf.byteLength) return;
      let size = this.buf.byteLength * 2;
      while (size < this.at + n) size *= 2;
      const next = new ArrayBuffer(size);
      new Uint8Array(next).set(this.u8.subarray(0, this.at));
      this.buf = next; this.u8 = new Uint8Array(next); this.dv = new DataView(next);
    }
    u16(v) { this.room(2); this.dv.setUint16(this.at, v, true); this.at += 2; }
    u32(v) { this.room(4); this.dv.setUint32(this.at, v >>> 0, true); this.at += 4; }
    i32(v) { this.room(4); this.dv.setInt32(this.at, v | 0, true); this.at += 4; }
    f32(v) { this.room(4); this.dv.setFloat32(this.at, v, true); this.at += 4; }
    f64(v) { this.room(8); this.dv.setFloat64(this.at, v, true); this.at += 8; }
    bytes(view) {
      const u8 = view instanceof Uint8Array ? view : ArrayBuffer.isView(view) ? new Uint8Array(view.buffer, view.byteOffset, view.byteLength) : new Uint8Array(view);
      this.u32(u8.byteLength); this.room(u8.byteLength); this.u8.set(u8, this.at); this.at += u8.byteLength;
    }
    json(value) { this.bytes(encoder.encode(JSON.stringify(value))); }
  }
  const encoder = new TextEncoder();
  const out = new Writer();
  let flushQueued = false;

  // One command: op, flags, then its fields (the length patched once they are written).
  function command(op, write) {
    out.u16(op); out.u16(0);
    const lengthAt = out.at; out.u32(0);
    write(out);
    out.dv.setUint32(lengthAt, out.at - lengthAt - 4, true);
    if (!flushQueued) { flushQueued = true; queueMicrotask(flush); }
  }

  function flush() {
    flushQueued = false;
    if (out.at) { pending.push(out.u8.slice(0, out.at)); out.reset(); }
    pump();
  }

  function pump() {
    while (pending.length) {
      const free = shared.findIndex((s) => !s.busy);
      if (free < 0) return;
      const target = shared[free];
      // Several chunks in one buffer when they fit; one too big for any buffer is split by the app's reader? No:
      // commands never span buffers, so a chunk larger than a buffer is sent in its own oversized message.
      let length = 0;
      while (pending.length && length + pending[0].byteLength <= target.u8.byteLength) {
        target.u8.set(pending[0], length); length += pending[0].byteLength; pending.shift();
      }
      if (!length) {
        // A single chunk bigger than a shared buffer (a huge texture): through the message itself.
        const chunk = pending.shift();
        host.postMessage({ gpuBridge: "inline", data: base64(chunk) });
        continue;
      }
      target.busy = true;
      host.postMessage({ gpuBridge: "flush", buffer: free, length });
    }
  }

  function base64(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    return btoa(s);
  }
  function unbase64(text) {
    const s = atob(text); const u8 = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) u8[i] = s.charCodeAt(i);
    return u8;
  }

  function expect() {
    const id = nextCallback++;
    const promise = new Promise((resolve, reject) => callbacks.set(id, { resolve, reject }));
    return { id, promise };
  }

  host.addEventListener("sharedbufferreceived", (event) => {
    const data = event.additionalData || {};
    if (data.gpuBridge !== "buffer") return;
    shared[data.index] = { u8: new Uint8Array(event.getBuffer()), busy: false };
    pump();
  });
  host.addEventListener("message", (event) => {
    const m = event.data;
    if (!m || typeof m !== "object" || !m.gpuBridge) return;
    switch (m.gpuBridge) {
      case "ready": ready = m.adapter; readyWaiters.splice(0).forEach((w) => w()); break;
      case "unavailable": unavailable = m.reason || "unavailable"; readyWaiters.splice(0).forEach((w) => w()); break;
      case "ack": if (shared[m.buffer]) shared[m.buffer].busy = false; pump(); break;
      case "reply": {
        const waiting = callbacks.get(m.callback);
        if (!waiting) break;
        callbacks.delete(m.callback);
        if (m.ok) waiting.resolve(m.data); else waiting.reject(m.error);
        break;
      }
      // One native device for the page: what it says goes to every device the page made.
      case "error": for (const d of devices.values()) d.uncaptured(m.kind, m.message); break;
      case "lost": for (const d of devices.values()) d.lose(m.reason, m.message); break;
    }
  });

  // ---------------------------------------------------------------- objects
  // Ids freed on the app's side once the page lets its object go.
  const released = new FinalizationRegistry((id) => command(22, (w) => w.u32(id)));
  class GPUObjectBase {
    constructor(label) { this.__id = nextId++; this.label = label || ""; released.register(this, this.__id); }
  }
  const idOf = (object) => (object ? object.__id : 0);
  // Descriptors with their objects replaced by ids (the app's JSON).
  function ids(value) {
    if (value === null || value === undefined) return value;
    if (value instanceof GPUObjectBase) return value.__id;
    if (Array.isArray(value)) return value.map(ids);
    if (ArrayBuffer.isView(value)) return Array.from(value);
    if (typeof value === "object") {
      const outObj = {};
      for (const key of Object.keys(value)) outObj[key] = ids(value[key]);
      return outObj;
    }
    return value;
  }
  const extent = (size) => (Array.isArray(size) || ArrayBuffer.isView(size) ? { width: size[0], height: size[1] ?? 1, depthOrArrayLayers: size[2] ?? 1 } : { width: size.width, height: size.height ?? 1, depthOrArrayLayers: size.depthOrArrayLayers ?? 1 });
  const origin = (o) => (!o ? { x: 0, y: 0, z: 0 } : Array.isArray(o) ? { x: o[0] ?? 0, y: o[1] ?? 0, z: o[2] ?? 0 } : { x: o.x ?? 0, y: o.y ?? 0, z: o.z ?? 0 });

  class GPUSupportedFeaturesBridge extends Set {}
  const limitsOf = (limits) => Object.freeze({ ...limits });

  class GPUBufferBridge extends GPUObjectBase {
    constructor(device, desc) {
      super(desc.label);
      this.device = device; this.size = desc.size; this.usage = desc.usage;
      this.mapState = desc.mappedAtCreation ? "mapped" : "unmapped";
      this.__mapped = desc.mappedAtCreation ? new ArrayBuffer(desc.size) : null;
      this.__mappedOffset = 0;
      this.__write = Boolean(desc.mappedAtCreation) || (desc.usage & GPUBufferUsage.MAP_WRITE) !== 0;
      command(1, (w) => { w.u32(this.__id); w.json({ size: desc.size, usage: desc.usage, mappedAtCreation: false, label: desc.label || "" }); });
    }
    async mapAsync(mode, offset = 0, size) {
      size ??= this.size - offset;
      if (mode & GPUMapMode.WRITE) {
        this.__mapped = new ArrayBuffer(size); this.__mappedOffset = offset; this.__write = true; this.mapState = "mapped";
        return;
      }
      this.mapState = "pending";
      const reply = expect();
      command(20, (w) => { w.u32(this.__id); w.u32(reply.id); w.f64(offset); w.f64(size); });
      flush();
      const data = await reply.promise;
      const u8 = unbase64(data);
      this.__mapped = u8.buffer; this.__mappedOffset = offset; this.__write = false; this.mapState = "mapped";
    }
    getMappedRange(offset = 0, size) {
      if (!this.__mapped) throw new DOMException("The buffer is not mapped", "OperationError");
      const start = offset - this.__mappedOffset;
      const length = size ?? this.__mapped.byteLength - start;
      if (start === 0 && length === this.__mapped.byteLength) return this.__mapped;
      return rangeOf(this, start, length);
    }
    unmap() {
      if (this.__mapped && this.__write) {
        const data = new Uint8Array(this.__mapped);
        const at = this.__mappedOffset;
        command(19, (w) => { w.u32(this.__id); w.f64(at); w.bytes(data); });
      }
      this.__ranges = null; this.__mapped = null; this.mapState = "unmapped";
    }
    destroy() { command(21, (w) => w.u32(this.__id)); }
  }
  // A sub-range of a mapped buffer: its own ArrayBuffer, copied back into the mapping at unmap.
  function rangeOf(buffer, start, size) {
    const length = size ?? buffer.__mapped.byteLength - start;
    const piece = new Uint8Array(length);
    piece.set(new Uint8Array(buffer.__mapped, start, length));
    (buffer.__ranges ??= []).push({ start, piece });
    const unmap = buffer.unmap.bind(buffer);
    if (!buffer.__patched) {
      buffer.__patched = true;
      buffer.unmap = function () {
        for (const r of this.__ranges || []) new Uint8Array(this.__mapped).set(r.piece, r.start);
        unmap();
      };
    }
    return piece.buffer;
  }

  class GPUTextureBridge extends GPUObjectBase {
    constructor(device, desc, canvasTexture = false) {
      super(desc.label);
      const size = extent(desc.size);
      Object.assign(this, { device, width: size.width, height: size.height, depthOrArrayLayers: size.depthOrArrayLayers, mipLevelCount: desc.mipLevelCount ?? 1, sampleCount: desc.sampleCount ?? 1, dimension: desc.dimension ?? "2d", format: desc.format, usage: desc.usage });
      if (!canvasTexture) command(2, (w) => { w.u32(this.__id); w.json(ids({ ...desc, size })); });
    }
    createView(desc = {}) {
      const view = new GPUTextureViewBridge(desc.label);
      command(3, (w) => { w.u32(view.__id); w.u32(this.__id); w.json(ids(desc)); });
      return view;
    }
    destroy() { command(21, (w) => w.u32(this.__id)); }
  }
  class GPUTextureViewBridge extends GPUObjectBase {}
  class GPUSamplerBridge extends GPUObjectBase {}
  class GPUBindGroupLayoutBridge extends GPUObjectBase {}
  class GPUPipelineLayoutBridge extends GPUObjectBase {}
  class GPUBindGroupBridge extends GPUObjectBase {}
  class GPUQuerySetBridge extends GPUObjectBase {
    constructor(desc) { super(desc.label); this.type = desc.type; this.count = desc.count; }
    destroy() { command(21, (w) => w.u32(this.__id)); }
  }
  class GPUShaderModuleBridge extends GPUObjectBase {
    async getCompilationInfo() {
      const reply = expect();
      command(29, (w) => { w.u32(this.__id); w.u32(reply.id); });
      flush();
      const data = await reply.promise.catch(() => ({ messages: [] }));
      return { messages: (data && data.messages) || [] };
    }
  }
  class GPUPipelineBridge extends GPUObjectBase {
    getBindGroupLayout(index) {
      const layout = new GPUBindGroupLayoutBridge();
      command(11, (w) => { w.u32(layout.__id); w.u32(this.__id); w.u32(index); });
      return layout;
    }
  }
  class GPUCommandBufferBridge extends GPUObjectBase {}
  class GPURenderBundleBridge extends GPUObjectBase {}

  // Render passes, compute passes and render bundle encoders: the same pass commands.
  class GPUPassBridge extends GPUObjectBase {
    setPipeline(p) { command(50, (w) => { w.u32(this.__id); w.u32(idOf(p)); }); }
    setBindGroup(index, group, offsets, start, length) {
      let list = offsets ? Array.from(offsets) : [];
      if (offsets && start !== undefined) list = Array.from(offsets).slice(start, start + length);
      command(51, (w) => { w.u32(this.__id); w.u32(index); w.u32(idOf(group)); w.u32(list.length); for (const o of list) w.u32(o); });
    }
    setVertexBuffer(slot, buffer, offset = 0, size) { command(52, (w) => { w.u32(this.__id); w.u32(slot); w.u32(idOf(buffer)); w.f64(offset); w.f64(size ?? -1); }); }
    setIndexBuffer(buffer, format, offset = 0, size) { command(53, (w) => { w.u32(this.__id); w.u32(idOf(buffer)); w.u32(format === "uint32" ? 1 : 0); w.f64(offset); w.f64(size ?? -1); }); }
    draw(vertexCount, instanceCount = 1, firstVertex = 0, firstInstance = 0) { command(54, (w) => { w.u32(this.__id); w.u32(vertexCount); w.u32(instanceCount); w.u32(firstVertex); w.u32(firstInstance); }); }
    drawIndexed(indexCount, instanceCount = 1, firstIndex = 0, baseVertex = 0, firstInstance = 0) { command(55, (w) => { w.u32(this.__id); w.u32(indexCount); w.u32(instanceCount); w.u32(firstIndex); w.i32(baseVertex); w.u32(firstInstance); }); }
    drawIndirect(buffer, offset) { command(56, (w) => { w.u32(this.__id); w.u32(idOf(buffer)); w.f64(offset); }); }
    drawIndexedIndirect(buffer, offset) { command(57, (w) => { w.u32(this.__id); w.u32(idOf(buffer)); w.f64(offset); }); }
    setViewport(x, y, width, height, minDepth, maxDepth) { command(58, (w) => { w.u32(this.__id); for (const v of [x, y, width, height, minDepth, maxDepth]) w.f32(v); }); }
    setScissorRect(x, y, width, height) { command(59, (w) => { w.u32(this.__id); w.u32(x); w.u32(y); w.u32(width); w.u32(height); }); }
    setBlendConstant(color) { const c = Array.isArray(color) ? color : [color.r, color.g, color.b, color.a]; command(60, (w) => { w.u32(this.__id); for (const v of c) w.f64(v); }); }
    setStencilReference(reference) { command(61, (w) => { w.u32(this.__id); w.u32(reference); }); }
    beginOcclusionQuery(index) { command(62, (w) => { w.u32(this.__id); w.u32(index); }); }
    endOcclusionQuery() { command(63, (w) => w.u32(this.__id)); }
    executeBundles(bundles) { const list = Array.from(bundles); command(64, (w) => { w.u32(this.__id); w.u32(list.length); for (const b of list) w.u32(idOf(b)); }); }
    end() { command(65, (w) => w.u32(this.__id)); }
    dispatchWorkgroups(x, y = 1, z = 1) { command(66, (w) => { w.u32(this.__id); w.u32(x); w.u32(y); w.u32(z); }); }
    dispatchWorkgroupsIndirect(buffer, offset) { command(67, (w) => { w.u32(this.__id); w.u32(idOf(buffer)); w.f64(offset); }); }
    pushDebugGroup() {}
    popDebugGroup() {}
    insertDebugMarker() {}
    finish(desc = {}) {
      // A render bundle encoder's finish.
      const bundle = new GPURenderBundleBridge(desc.label);
      command(38, (w) => { w.u32(bundle.__id); w.u32(this.__id); });
      return bundle;
    }
  }

  class GPUCommandEncoderBridge extends GPUObjectBase {
    beginRenderPass(desc) {
      const pass = new GPUPassBridge(desc.label);
      command(13, (w) => { w.u32(pass.__id); w.u32(this.__id); w.json(ids(desc)); });
      return pass;
    }
    beginComputePass(desc = {}) {
      const pass = new GPUPassBridge(desc.label);
      command(14, (w) => { w.u32(pass.__id); w.u32(this.__id); w.json(ids(desc)); });
      return pass;
    }
    copyBufferToBuffer(src, srcOffset, dst, dstOffset, size) {
      // The shorter form (src, dst[, size]).
      if (srcOffset instanceof GPUObjectBase) { size = dst; dst = srcOffset; srcOffset = 0; dstOffset = 0; }
      size ??= src.size - srcOffset;
      command(31, (w) => { w.u32(this.__id); w.u32(idOf(src)); w.f64(srcOffset); w.u32(idOf(dst)); w.f64(dstOffset); w.f64(size); });
    }
    copyBufferToTexture(source, destination, size) { command(32, (w) => { w.u32(this.__id); w.json(ids({ source, destination: { ...destination, origin: origin(destination.origin) }, size: extent(size) })); }); }
    copyTextureToBuffer(source, destination, size) { command(33, (w) => { w.u32(this.__id); w.json(ids({ source: { ...source, origin: origin(source.origin) }, destination, size: extent(size) })); }); }
    copyTextureToTexture(source, destination, size) { command(34, (w) => { w.u32(this.__id); w.json(ids({ source: { ...source, origin: origin(source.origin) }, destination: { ...destination, origin: origin(destination.origin) }, size: extent(size) })); }); }
    resolveQuerySet(querySet, first, count, destination, offset) { command(35, (w) => { w.u32(this.__id); w.u32(idOf(querySet)); w.u32(first); w.u32(count); w.u32(idOf(destination)); w.f64(offset); }); }
    clearBuffer(buffer, offset = 0, size) { command(36, (w) => { w.u32(this.__id); w.u32(idOf(buffer)); w.f64(offset); w.f64(size ?? -1); }); }
    pushDebugGroup() {}
    popDebugGroup() {}
    insertDebugMarker() {}
    finish(desc = {}) {
      const buffer = new GPUCommandBufferBridge(desc.label);
      command(15, (w) => { w.u32(buffer.__id); w.u32(this.__id); });
      return buffer;
    }
  }

  // Pixels of an image, a bitmap or a canvas, as RGBA8 (flipped, premultiplied as asked).
  const scratch = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(1, 1) : document.createElement("canvas");
  const scratch2d = scratch.getContext("2d", { willReadFrequently: true });
  function pixelsOf(source, flipY, premultiply, width, height) {
    if (source.data && source.width && !source.getContext && !(source instanceof ImageBitmap) && source.data.length !== undefined) {
      return new Uint8Array(source.data.buffer, source.data.byteOffset, source.data.byteLength);
    }
    scratch.width = width; scratch.height = height;
    scratch2d.save();
    scratch2d.clearRect(0, 0, width, height);
    if (flipY) { scratch2d.translate(0, height); scratch2d.scale(1, -1); }
    scratch2d.drawImage(source, 0, 0, width, height);
    scratch2d.restore();
    const data = scratch2d.getImageData(0, 0, width, height).data;
    if (premultiply) for (let i = 0; i < data.length; i += 4) { const a = data[i + 3] / 255; data[i] *= a; data[i + 1] *= a; data[i + 2] *= a; }
    return new Uint8Array(data.buffer);
  }

  class GPUQueueBridge {
    constructor(device) { this.device = device; this.label = ""; }
    submit(buffers) {
      const list = Array.from(buffers);
      command(16, (w) => { w.u32(list.length); for (const b of list) w.u32(idOf(b)); });
      if (this.device.__canvasUsed) { this.device.__canvasUsed = false; command(25, () => {}); }
      flush();
    }
    writeBuffer(buffer, bufferOffset, data, dataOffset = 0, size) {
      const bytesPer = ArrayBuffer.isView(data) && data.BYTES_PER_ELEMENT ? data.BYTES_PER_ELEMENT : 1;
      const base = ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : new Uint8Array(data);
      const start = dataOffset * bytesPer;
      const length = size === undefined ? base.byteLength - start : size * bytesPer;
      const slice = base.subarray(start, start + length);
      command(17, (w) => { w.u32(idOf(buffer)); w.f64(bufferOffset); w.bytes(slice); });
    }
    writeTexture(destination, data, layout, size) {
      const bytes = ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : new Uint8Array(data);
      command(18, (w) => { w.json(ids({ destination: { ...destination, origin: origin(destination.origin) }, layout: { offset: layout.offset ?? 0, bytesPerRow: layout.bytesPerRow, rowsPerImage: layout.rowsPerImage }, size: extent(size) })); w.bytes(bytes); });
    }
    copyExternalImageToTexture(source, destination, copySize) {
      const size = extent(copySize);
      const pixels = pixelsOf(source.source, Boolean(source.flipY), Boolean(destination.premultipliedAlpha), size.width, size.height);
      this.writeTexture({ texture: destination.texture, mipLevel: destination.mipLevel ?? 0, origin: destination.origin, aspect: "all" }, pixels, { offset: 0, bytesPerRow: size.width * 4, rowsPerImage: size.height }, size);
    }
    onSubmittedWorkDone() {
      const reply = expect();
      command(26, (w) => w.u32(reply.id));
      flush();
      return reply.promise.then(() => undefined);
    }
  }

  class GPUDeviceBridge extends EventTarget {
    constructor(adapter) {
      super();
      this.__id = nextId++;
      this.label = "";
      this.features = adapter.features;
      this.limits = adapter.limits;
      this.adapterInfo = adapter.info;
      this.queue = new GPUQueueBridge(this);
      this.__canvasUsed = false;
      let lose;
      this.lost = new Promise((resolve) => (lose = resolve));
      this.__lose = lose;
      this.onuncapturederror = null;
      devices.set(this.__id, this);
    }
    uncaptured(kind, message) {
      const ErrorType = kind === "out-of-memory" ? (window.GPUOutOfMemoryError || Error) : kind === "internal" ? (window.GPUInternalError || Error) : (window.GPUValidationError || Error);
      const error = new ErrorType(message);
      const event = typeof GPUUncapturedErrorEvent !== "undefined" ? new GPUUncapturedErrorEvent("uncapturederror", { error }) : Object.assign(new Event("uncapturederror"), { error });
      this.dispatchEvent(event);
      if (typeof this.onuncapturederror === "function") this.onuncapturederror(event);
      else console.error(`WebGPU (natif) : ${message}`);
    }
    lose(reason, message) { this.__lose({ reason: reason || "unknown", message: message || "" }); }
    destroy() { this.lose("destroyed", ""); }
    createBuffer(desc) { return new GPUBufferBridge(this, desc); }
    createTexture(desc) { return new GPUTextureBridge(this, desc); }
    createSampler(desc = {}) { const s = new GPUSamplerBridge(desc.label); command(4, (w) => { w.u32(s.__id); w.json(desc); }); return s; }
    createBindGroupLayout(desc) { const l = new GPUBindGroupLayoutBridge(desc.label); command(5, (w) => { w.u32(l.__id); w.json(ids(desc)); }); return l; }
    createPipelineLayout(desc) { const l = new GPUPipelineLayoutBridge(desc.label); command(6, (w) => { w.u32(l.__id); w.json(ids(desc)); }); return l; }
    createBindGroup(desc) { const g = new GPUBindGroupBridge(desc.label); command(7, (w) => { w.u32(g.__id); w.json(ids(desc)); }); return g; }
    createShaderModule(desc) { const m = new GPUShaderModuleBridge(desc.label); command(8, (w) => { w.u32(m.__id); w.json({ code: desc.code, label: desc.label || "" }); }); return m; }
    createQuerySet(desc) { const q = new GPUQuerySetBridge(desc); command(30, (w) => { w.u32(q.__id); w.json(desc); }); return q; }
    createRenderPipeline(desc) { const p = new GPUPipelineBridge(desc.label); command(9, (w) => { w.u32(p.__id); w.u32(0); w.json(ids(desc)); }); return p; }
    createComputePipeline(desc) { const p = new GPUPipelineBridge(desc.label); command(10, (w) => { w.u32(p.__id); w.u32(0); w.json(ids(desc)); }); return p; }
    createRenderPipelineAsync(desc) { return this.__async(9, desc); }
    createComputePipelineAsync(desc) { return this.__async(10, desc); }
    __async(op, desc) {
      const p = new GPUPipelineBridge(desc.label);
      const reply = expect();
      command(op, (w) => { w.u32(p.__id); w.u32(reply.id); w.json(ids(desc)); });
      flush();
      return reply.promise.then(() => p, (message) => {
        const PipelineError = window.GPUPipelineError;
        throw PipelineError ? new PipelineError(String(message), { reason: "validation" }) : new Error(String(message));
      });
    }
    createCommandEncoder(desc = {}) { const e = new GPUCommandEncoderBridge(desc.label); command(12, (w) => w.u32(e.__id)); return e; }
    createRenderBundleEncoder(desc) { const e = new GPUPassBridge(desc.label); command(37, (w) => { w.u32(e.__id); w.json(desc); }); return e; }
    importExternalTexture() { throw new DOMException("Les textures vidéo ne passent pas par le pont WebGPU natif", "NotSupportedError"); }
    pushErrorScope(filter) { command(27, (w) => w.u32(filter === "out-of-memory" ? 1 : filter === "internal" ? 2 : 0)); }
    popErrorScope() {
      const reply = expect();
      command(28, (w) => w.u32(reply.id));
      flush();
      return reply.promise.then((error) => {
        if (!error) return null;
        const ErrorType = error.kind === "out-of-memory" ? (window.GPUOutOfMemoryError || Error) : (window.GPUValidationError || Error);
        return new ErrorType(error.message);
      });
    }
  }

  // The game's canvas: its WebGPU context is the app's picture, shown under the page.
  class GPUCanvasContextBridge {
    constructor(canvas) { this.canvas = canvas; this.__device = null; this.__config = null; }
    configure(config) {
      this.__device = config.device;
      this.__config = { ...config };
      this.__size = [0, 0];
      this.__resize();
    }
    __resize() {
      const w = Math.max(1, this.canvas.width | 0), h = Math.max(1, this.canvas.height | 0);
      if (this.__size[0] === w && this.__size[1] === h) return;
      this.__size = [w, h];
      const c = this.__config;
      command(23, (wr) => wr.json({ width: w, height: h, format: c.format, alphaMode: c.alphaMode || "opaque", usage: c.usage ?? GPUTextureUsage.RENDER_ATTACHMENT }));
    }
    unconfigure() { this.__device = null; }
    getConfiguration() { return this.__config; }
    getCurrentTexture() {
      if (!this.__device) throw new DOMException("The canvas is not configured", "InvalidStateError");
      this.__resize();
      if (!this.__current) {
        const c = this.__config;
        this.__current = new GPUTextureBridge(this.__device, { size: this.__size, format: c.format, usage: c.usage ?? GPUTextureUsage.RENDER_ATTACHMENT, label: "canvas" }, true);
        command(24, (w) => w.u32(this.__current.__id));
        this.__device.__canvasUsed = true;
        // One texture per frame, as in the browser: a new one after this task.
        setTimeout(() => (this.__current = null), 0);
      }
      return this.__current;
    }
  }

  // Where the canvas is in the page (CSS pixels, of the page's viewport): the app puts its picture there.
  let placed = "";
  function watchPlace(canvas) {
    const say = () => {
      if (!canvas.isConnected) return;
      const r = canvas.getBoundingClientRect();
      const place = [r.left, r.top, r.width, r.height, innerWidth, innerHeight].map((v) => Math.round(v * 100) / 100).join(",");
      if (place === placed) return;
      placed = place;
      host.postMessage({ gpuBridge: "place", rect: [r.left, r.top, r.width, r.height], viewport: [innerWidth, innerHeight] });
    };
    const loop = () => { say(); requestAnimationFrame(loop); };
    requestAnimationFrame(loop);
  }

  const getContext = HTMLCanvasElement.prototype.getContext;
  const contexts = new WeakMap();
  function useBridgeContext(canvas) {
    let ctx = contexts.get(canvas);
    if (!ctx) {
      ctx = new GPUCanvasContextBridge(canvas);
      contexts.set(canvas, ctx);
      // The page shows the app's picture through it: nothing of its own behind it (the canvas and what
      // holds it, up to the document), and the app told where the canvas is.
      const clear = () => {
        for (let el = canvas; el; el = el.parentElement) el.style.setProperty("background", "transparent", "important");
        document.documentElement.style.setProperty("background", "transparent", "important");
        if (document.body) document.body.style.setProperty("background", "transparent", "important");
      };
      clear();
      if (!canvas.isConnected) requestAnimationFrame(clear);
      watchPlace(canvas);
    }
    return ctx;
  }

  class GPUAdapterBridge {
    constructor(adapter) {
      this.features = new GPUSupportedFeaturesBridge(adapter.features || []);
      this.limits = limitsOf(adapter.limits || {});
      this.info = Object.freeze({ vendor: "microsoft", architecture: "", device: "", description: "Cartouche Dawn (DirectX 12)", ...(adapter.info || {}) });
      this.isFallbackAdapter = false;
    }
    async requestAdapterInfo() { return this.info; }
    async requestDevice(desc = {}) {
      const features = new GPUSupportedFeaturesBridge((desc.requiredFeatures || []).filter((f) => this.features.has(f)));
      const device = new GPUDeviceBridge({ features, limits: limitsOf({ ...this.limits }), info: this.info });
      return device;
    }
  }

  function whenReady() {
    if (ready || unavailable) return Promise.resolve();
    return new Promise((resolve) => readyWaiters.push(resolve));
  }

  const bridgeGpu = {
    async requestAdapter() {
      await whenReady();
      if (!ready) return realGpu ? realGpu.requestAdapter.apply(realGpu, arguments) : null;
      return new GPUAdapterBridge(ready);
    },
    getPreferredCanvasFormat: () => "bgra8unorm",
    wgslLanguageFeatures: new Set(),
  };

  Object.defineProperty(Navigator.prototype, "gpu", { configurable: true, get: () => bridgeGpu });
  HTMLCanvasElement.prototype.getContext = function (kind, options) {
    if (kind === "webgpu" && ready) return useBridgeContext(this);
    return getContext.call(this, kind, options);
  };
  window.__cartoucheGpuBridge = { flush, stats: () => ({ shared: shared.length, pending: pending.length, ready: Boolean(ready), unavailable }) };
  host.postMessage({ gpuBridge: "hello" });
})();
