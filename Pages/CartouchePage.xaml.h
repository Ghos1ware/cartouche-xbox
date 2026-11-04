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

#include <atomic>
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
