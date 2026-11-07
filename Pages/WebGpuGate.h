#pragma once

namespace moonlight_xbox_dx {
	// WebGPU on the console (in every page that runs a game: the Studio's preview, the web games).
	// The browser starts with --enable-unsafe-webgpu --use-webgpu-adapter=d3d11 (CartouchePage::Start): on
	// D3D12 the console's driver fails any render pipeline WebGPU makes (DXGI_ERROR_DRIVER_INTERNAL_ERROR,
	// the whole GPU process down with it), while Dawn's D3D11 backend draws on the real GPU, in WebGPU's
	// « compatibility » feature level (three.js asks for it on its own). So:
	// - an adapter asked in compatibility gets it; one asked in core gets none (on D3D11 it would be
	//   SwiftShader, the CPU): the game falls back to WebGL 2;
	// - no « timestamp-query » (the GPU timers brought the GPU process down too, as WebGL's did).
	inline const wchar_t* XBOX_WEBGPU_GATE = LR"JS((() => {
  try {
    const G = window.GPU && GPU.prototype;
    if (G) {
      const requestAdapter = G.requestAdapter;
      G.requestAdapter = function (options) {
        if (!options || options.featureLevel !== "compatibility" || options.forceFallbackAdapter) return Promise.resolve(null);
        return requestAdapter.call(this, options);
      };
    }
    const A = window.GPUAdapter && GPUAdapter.prototype;
    if (!A) return;
    const features = Object.getOwnPropertyDescriptor(A, "features");
    Object.defineProperty(A, "features", { configurable: true, get() {
      const kept = new Set([...features.get.call(this)].filter((name) => name !== "timestamp-query"));
      return { has: (name) => kept.has(name), forEach: (each, self) => kept.forEach(each, self), keys: () => kept.keys(), values: () => kept.values(), entries: () => kept.entries(), [Symbol.iterator]: () => kept[Symbol.iterator](), get size() { return kept.size; } };
    } });
    const requestDevice = A.requestDevice;
    A.requestDevice = function (descriptor) {
      if (descriptor && descriptor.requiredFeatures) descriptor = Object.assign({}, descriptor, { requiredFeatures: [...descriptor.requiredFeatures].filter((name) => name !== "timestamp-query") });
      return requestDevice.call(this, descriptor);
    };
  } catch (e) {}
})();)JS";

	// The browser's switches for it (read once, before the first WebView2 starts).
	inline const wchar_t* XBOX_WEBGPU_SWITCHES = L" --enable-unsafe-webgpu --use-webgpu-adapter=d3d11";
}
