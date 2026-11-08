# The WebGPU bridge: the game's page to native Dawn (DirectX 12)

The page keeps its JavaScript, its DOM, its sound and its controller in the WebView2; only `navigator.gpu`
is replaced by `Assets/webgpu-bridge.js`, which records every WebGPU call as a command. The app replays them on
Dawn (`Bridge/GpuBridge.cpp`), on DirectX 12, and shows the picture in a `SwapChainPanel` under the page (its
background transparent). See `GamesLibrary/docs/XBOX-NATIVE-WEBGPU.md` for why.

## Transport

- The app makes shared buffers (`CoreWebView2SharedBuffer`, read-write) and posts them to the page
  (`PostSharedBufferToScript`, additional data `{ "gpuBridge": "buffer", "index": n }`).
- The page writes its commands into a local growing buffer, then on flush copies them into a free shared buffer
  and posts `{ gpuBridge: "flush", buffer: n, length }`. The app replays it, then answers
  `{ gpuBridge: "ack", buffer: n }`: the buffer is free again. Flushes are replayed in order.
- The app answers asynchronous calls by `callback` id: `{ gpuBridge: "reply", callback, ok, error?, data? }`.
  `data` of a mapped buffer is base64.
- The app says its adapter once its buffers are posted: `{ gpuBridge: "ready", adapter: { features, limits, info } }`
  or `{ gpuBridge: "unavailable", reason }` (the page then keeps the browser's own WebGPU).
- Errors the app sees (validation, out of memory) come as `{ gpuBridge: "error", device, kind, message }`;
  a lost device as `{ gpuBridge: "lost", device, reason, message }`.

## Commands

Little-endian. Each command: `u16 op`, `u16 flags (0)`, `u32 length` of what follows, then its fields.
Ids are `u32`, chosen by the page (the WebGPU calls that make objects are synchronous): 0 is « none ».
`json` is `u32 byteLength` then UTF-8; descriptors keep WebGPU's own names, objects replaced by their ids
(`{ "layout": 12 }`, `"auto"` kept). `bytes` is `u32 byteLength` then the bytes.

| op | name | fields |
|---|---|---|
| 1 | createBuffer | id, json desc (size, usage, mappedAtCreation, label) |
| 2 | createTexture | id, json desc |
| 3 | createView | id, texture, json desc |
| 4 | createSampler | id, json desc |
| 5 | createBindGroupLayout | id, json desc |
| 6 | createPipelineLayout | id, json desc |
| 7 | createBindGroup | id, json desc |
| 8 | createShaderModule | id, json desc (code) |
| 9 | createRenderPipeline | id, callback (0: synchronous), json desc |
| 10 | createComputePipeline | id, callback, json desc |
| 11 | getBindGroupLayout | id, pipeline, index |
| 12 | createCommandEncoder | id |
| 13 | beginRenderPass | id, encoder, json desc |
| 14 | beginComputePass | id, encoder, json desc |
| 15 | finish | id (command buffer), encoder |
| 16 | submit | count, ids… |
| 17 | writeBuffer | buffer, offset (f64), bytes |
| 18 | writeTexture | json destination+layout+size, bytes |
| 19 | bufferData | buffer, offset (f64), bytes — what a mapped-for-writing range held at `unmap` |
| 20 | mapRead | buffer, callback, offset (f64), size (f64) |
| 21 | destroy | id |
| 22 | release | id — the page's object is gone (garbage collected) |
| 23 | configureCanvas | json (width, height, format, alphaMode) |
| 24 | currentTexture | id |
| 25 | present | — |
| 26 | workDone | callback |
| 27 | pushErrorScope | filter (u32: 0 validation, 1 out-of-memory, 2 internal) |
| 28 | popErrorScope | callback |
| 29 | compilationInfo | shader module, callback |
| 30 | createQuerySet | id, json desc |
| 31 | copyBufferToBuffer | encoder, src, srcOffset (f64), dst, dstOffset (f64), size (f64) |
| 32 | copyBufferToTexture | encoder, json (source, destination, size) |
| 33 | copyTextureToBuffer | encoder, json |
| 34 | copyTextureToTexture | encoder, json |
| 35 | resolveQuerySet | encoder, querySet, first, count, destination, offset (f64) |
| 36 | clearBuffer | encoder, buffer, offset (f64), size (f64; < 0: to the end) |
| 37 | createRenderBundleEncoder | id, json desc |
| 38 | finishBundle | id (bundle), bundle encoder |
| 50 | setPipeline | pass, pipeline |
| 51 | setBindGroup | pass, index, group, count, offsets (u32)… |
| 52 | setVertexBuffer | pass, slot, buffer, offset (f64), size (f64; < 0: to the end) |
| 53 | setIndexBuffer | pass, buffer, format (u32: 0 uint16, 1 uint32), offset (f64), size (f64) |
| 54 | draw | pass, vertexCount, instanceCount, firstVertex, firstInstance |
| 55 | drawIndexed | pass, indexCount, instanceCount, firstIndex, baseVertex (i32), firstInstance |
| 56 | drawIndirect | pass, buffer, offset (f64) |
| 57 | drawIndexedIndirect | pass, buffer, offset (f64) |
| 58 | setViewport | pass, x, y, w, h, minDepth, maxDepth (f32) |
| 59 | setScissorRect | pass, x, y, w, h |
| 60 | setBlendConstant | pass, r, g, b, a (f64) |
| 61 | setStencilReference | pass, reference |
| 62 | beginOcclusionQuery | pass, index |
| 63 | endOcclusionQuery | pass |
| 64 | executeBundles | pass, count, ids… |
| 65 | end | pass |
| 66 | dispatch | pass, x, y, z |
| 67 | dispatchIndirect | pass, buffer, offset (f64) |
| 68 | pushDebugGroup / popDebugGroup / insertDebugMarker | (ignored) |

`pass` is a render pass, a compute pass or a render bundle encoder: the app knows which by its id.
