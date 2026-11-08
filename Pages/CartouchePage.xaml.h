//
// CartouchePage.xaml.h
// Cartouche's home: its interface from the site in a WebView2, and the CartoucheXbox channel
// that gives the page Moonlight's PCs, pairing, games and streams.
//

#pragma once

// clang-format off
#include "pch.h"
// clang-format on
#include "Common/DeviceWatcherHelper.h"
#include "Pages/CartouchePage.g.h"
#include "State/ApplicationState.h"
#include "Bridge/GpuBridge.h"

#include <atomic>
#include <memory>
#include <set>
#include <vector>

namespace moonlight_xbox_dx {

	[Windows::Foundation::Metadata::WebHostHidden] public ref class CartouchePage sealed {
	public:
		CartouchePage();
		void OnStateLoaded();

	protected:
		virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
		virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

	private:
		void Start();
		void OnWebMessage(Microsoft::Web::WebView2::Core::CoreWebView2^ sender, Microsoft::Web::WebView2::Core::CoreWebView2WebMessageReceivedEventArgs^ args);
		void OnNavigationStarting(Microsoft::Web::WebView2::Core::CoreWebView2^ sender, Microsoft::Web::WebView2::Core::CoreWebView2NavigationStartingEventArgs^ args);
		void OnNewWindow(Microsoft::Web::WebView2::Core::CoreWebView2^ sender, Microsoft::Web::WebView2::Core::CoreWebView2NewWindowRequestedEventArgs^ args);
		void OnBackRequested(Platform::Object^ sender, Windows::UI::Core::BackRequestedEventArgs^ args);

		// The loading screen (Loader in the XAML): up from the splash until the interface is drawn.
		void WatchReady();
		void Reveal();
		Windows::UI::Xaml::DispatcherTimer^ readyTimer;
		ULONGLONG shownAt = 0;
		bool checking = false;
		bool revealed = false;

		concurrency::task<Windows::Data::Json::IJsonValue^> Call(Platform::String^ method, Windows::Data::Json::JsonArray^ args);
		void Answer(int id, Windows::Data::Json::IJsonValue^ value);
		void Fail(int id, Platform::String^ error);
		void Event(Platform::String^ name, Windows::Data::Json::IJsonValue^ data);
		void Post(Windows::Data::Json::JsonObject^ message);

		// Cartouche Studio's live preview (Preview in the XAML): the game the PC serves on this network
		// (its lan-preview.ts), its certificate pinned by the fingerprint the page got through the
		// player's account. The controller stays the page's until « Jouer »; then View brings it back.
		concurrency::task<Windows::Data::Json::IJsonValue^> PreviewCall(const std::wstring& name, Windows::Data::Json::JsonArray^ args);
		void WirePreview();
		void PreviewPlaying(bool on);
		void PreviewEvent(Platform::String^ kind, Platform::String^ detail);
		void PreviewLayout();
		void DevtoolsCommand(const std::string& text);
		void Redraw(Windows::UI::Xaml::UIElement^ view);
		// Where the page's stage is (previewPlace, in this page's units) and whether it shows there.
		double placeX = 0, placeY = 0, placeWidth = 1, placeHeight = 1;
		bool placeVisible = false;
		int playTurn = 0;
		// The preview's GPU crashes, and when the last was (a second one soon after: no more reloading).
		int previewCrashes = 0;
		ULONGLONG previewCrashAt = 0;
		Platform::String^ previewOrigin;
		Platform::String^ previewFingerprint;
		bool previewWired = false;
		bool previewLoaded = false;
		std::atomic<bool> previewPlaying{ false };

		// The game's WebGPU drawn natively (Bridge/PROTOCOL.md): Dawn on DirectX 12 into GpuPanel.
		void GpuTriangle();
		void GpuTicking(bool on);
		std::unique_ptr<GpuBridge> gpu;
		Windows::Foundation::EventRegistrationToken gpuTick{};
		bool gpuTicking = false;
		// The preview through the bridge (the shim injected, its page transparent), or the browser's own WebGPU.
		bool GpuStart();
		void GpuStop();
		void GpuMessage(Microsoft::Web::WebView2::Core::CoreWebView2^ core, Windows::Data::Json::JsonObject^ message);
		void GpuLayout();
		bool gpuNative = false;
		bool gpuStarted = false;
		bool gpuAvailable = false;
		bool gpuDrawing = false;
		std::string gpuAdapter, gpuReason;
		std::vector<Microsoft::Web::WebView2::Core::CoreWebView2SharedBuffer^> gpuBuffers;
		std::vector<Windows::Foundation::IMemoryBufferReference^> gpuBufferRefs;
		std::vector<uint8_t*> gpuBufferData;
		std::vector<uint64_t> gpuBufferSize;
		// Where the game's canvas is, in the preview's CSS pixels, and the preview's own size in them.
		double gpuRect[4] = { 0, 0, 0, 0 }, gpuViewport[2] = { 0, 0 };

		MoonlightHost^ Host(Platform::String^ uuid);
		Windows::Data::Json::JsonArray^ Computers();
		void Watch(MoonlightHost^ host);
		void Play(MoonlightHost^ host, int appId, Platform::String^ name);

		// mDNS discovery of the PCs on the network, as in HostSelectorPage.
		void StartDiscovery();
		void OnDeviceAdded(Windows::Devices::Enumeration::DeviceWatcher^ sender, Windows::Devices::Enumeration::DeviceInformation^ info);
		void OnDeviceUpdated(Windows::Devices::Enumeration::DeviceWatcher^ sender, Windows::Devices::Enumeration::DeviceInformationUpdate^ info);
		void TryAddDiscoveredHost(Windows::Devices::Enumeration::DeviceInformation^ info);

		ApplicationState^ state;
		DeviceWatcherHelper^ deviceWatcherHelper;
		Platform::Collections::Map<Platform::String^, Windows::Devices::Enumeration::DeviceInformation^>^ discoveredDevices;
		std::shared_ptr<std::set<std::string>> probing;
		std::vector<MoonlightHost^> watched;
		std::atomic<bool> polling{ false };
		bool started = false;
		Windows::Foundation::EventRegistrationToken backToken;
	};
}
