//
// CartouchePage.xaml.cpp
// Cartouche for Xbox's home. The interface is Cartouche's own, served by the site and shown in a
// WebView2 (it stays closed source, as in Cartouche for Android). The page talks to the app
// through `CartoucheXbox`, a message channel this page puts on documents of the site's origin
// only: the PCs Moonlight finds, pairing, a PC's games, streaming them with Moonlight's player,
// and the PC's Ma collection from Cartouche itself (its API on port 47975, mutual TLS with the
// certificate Moonlight paired). The calls and events are cartouche-android's, one for one.
//

// clang-format off
#include "pch.h"
// clang-format on
#include "CartouchePage.xaml.h"
#include "HostSelectorPage.xaml.h"
#include "HostSettingsPage.xaml.h"
#include "StreamPage.xaml.h"
#include "WebGamePage.xaml.h"
#include "State/MoonlightClient.h"
#include "State/StreamConfiguration.h"
#include "Utils.hpp"

#include <Ws2tcpip.h>
#include <winsock2.h>

// After winsock2: curl.h needs its SOCKET.
extern "C" {
#include <libgamestream/errors.h>
#include <libgamestream/http.h>
}

#include <cmath>
#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

using namespace moonlight_xbox_dx;

using namespace concurrency;
using namespace Microsoft::Web::WebView2::Core;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Foundation;
using namespace Windows::Security::Cryptography;
using namespace Windows::System;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Navigation;

namespace {
	const wchar_t* ORIGIN = L"https://cartouche.gg";
	const wchar_t* HOME = L"https://cartouche.gg/?shell=xbox";
	const int API_PORT = 47975;
	// The covers the page shows, served from their folders as files (SetVirtualHostNameToFolderMapping):
	// inlined in the channel, a PC's few hundred covers took the Xbox's memory.
	const wchar_t* COVERS_HOST = L"covers.cartouche.example";
	const wchar_t* PC_COVERS_HOST = L"pc-covers.cartouche.example";

	// On documents of the site only, top frame only: the page sees `CartoucheXbox`, with the
	// same shape as Android's channel (postMessage, and "message" events carrying strings).
	const wchar_t* CHANNEL = LR"JS((() => {
  if (location.origin !== "https://cartouche.gg" || window.top !== window || !window.chrome || !window.chrome.webview) return;
  const webview = window.chrome.webview;
  const target = new EventTarget();
  webview.addEventListener("message", (event) => target.dispatchEvent(new MessageEvent("message", { data: event.data })));
  Object.defineProperty(window, "CartoucheXbox", {
    value: Object.freeze({
      postMessage: (message) => webview.postMessage(String(message)),
      addEventListener: (type, listener) => target.addEventListener(type, listener),
    }),
  });
})();)JS";

	// No Cartouche answers on the PC: an older one, the firewall, or another server.
	struct Unavailable : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	bool Ours(String^ uri) {
		if (uri == nullptr) return false;
		std::wstring url(uri->Data());
		std::wstring origin(ORIGIN);
		return url == origin || url.rfind(origin + L"/", 0) == 0 || url.rfind(origin + L"?", 0) == 0;
	}

	String^ Wide(const std::string& text) { return Utils::StringFromStdString(text); }

	// The PC's address without Moonlight's port: "192.168.1.20", "[2001:db8::10]".
	std::string HostPart(String^ hostname) {
		std::string host = Utils::PlatformStringToStdString(hostname);
		if (!host.empty() && host[0] == '[') {
			auto end = host.find(']');
			return end == std::string::npos ? host : host.substr(0, end + 1);
		}
		auto colon = host.find(':');
		if (colon != std::string::npos && host.find(':', colon + 1) == std::string::npos) return host.substr(0, colon);
		return host;
	}

	String^ Uuid(MoonlightHost^ host) {
		return host->InstanceId != nullptr && !host->InstanceId->IsEmpty() ? host->InstanceId : host->LastHostname;
	}

	void WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes) {
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	// The page's address of a cover kept in one of the served folders; null when there is none.
	IJsonValue^ Served(const wchar_t* host, const std::wstring& file, const std::wstring& path) {
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) || (data.nFileSizeHigh == 0 && data.nFileSizeLow == 0)) return JsonValue::CreateNullValue();
		return JsonValue::CreateStringValue(ref new String((L"https://" + std::wstring(host) + L"/" + path).c_str()));
	}

	std::wstring LocalFolder(const std::wstring& sub) {
		std::wstring path = std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\" + sub + L"\\";
		CreateDirectoryW(path.c_str(), NULL);
		return path;
	}

	// Cartouche's API on the PC, with the client certificate Moonlight paired (libgamestream's).
	std::vector<uint8_t> ApiGet(MoonlightHost^ host, const std::string& path) {
		std::string url = "https://" + HostPart(host->LastHostname) + ":" + std::to_string(API_PORT) + path;
		CURL* curl = get_curl_handle();
		if (curl == nullptr) throw std::runtime_error("curl");
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
		PHTTP_DATA data = http_create_data();
		int status = http_request(curl, const_cast<char*>(url.c_str()), data);
		long code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		http_cleanup(curl);
		if (status != GS_OK) {
			http_free_data(data);
			if (code == 403) throw Unavailable("Cartouche does not know this Xbox");
			if (code == 0) throw Unavailable("Cartouche does not answer on " + HostPart(host->LastHostname));
			throw std::runtime_error("Cartouche answered " + std::to_string(code));
		}
		std::vector<uint8_t> bytes(data->memory, data->memory + data->size);
		http_free_data(data);
		return bytes;
	}

	JsonObject^ ApiJson(MoonlightHost^ host, const std::string& path) {
		auto bytes = ApiGet(host, path);
		JsonObject^ json;
		if (!JsonObject::TryParse(Wide(std::string(bytes.begin(), bytes.end())), &json)) throw std::runtime_error("Cartouche answered something else than JSON");
		return json;
	}

	std::string GameId(String^ id) {
		std::string value = Utils::PlatformStringToStdString(id);
		if (!std::regex_match(value, std::regex("[a-z0-9-]+"))) throw std::invalid_argument("Not a game id: " + value);
		return value;
	}

	// ---- The stream's settings (Cartouche's screen, ui/PhoneStream.tsx) over Moonlight UWP's ----

	// The PC they apply to: the paired one, else the first.
	MoonlightHost^ SettingsHost(ApplicationState^ state) {
		for (auto host : state->SavedHosts) {
			if (host->Paired) return host;
		}
		if (state->SavedHosts->Size > 0) return state->SavedHosts->GetAt(0);
		throw std::runtime_error("Aucun PC");
	}

	// Moonlight's default bitrate (the Shield's table, by resolution, then frame rate), in kb/s.
	int AutoBitrate(int width, int height, int fps) {
		const int pixels[] = { 1280 * 720, 1920 * 1080, 2560 * 1440, 3840 * 2160 };
		const float factors[] = { 5.0f, 10.0f, 20.0f, 40.0f };
		int count = width * height;
		float resolution = factors[3];
		if (count <= pixels[0]) resolution = factors[0];
		else {
			for (int i = 1; i < 4; i++) {
				if (count <= pixels[i]) {
					resolution = factors[i - 1] + (float)(count - pixels[i - 1]) / (pixels[i] - pixels[i - 1]) * (factors[i] - factors[i - 1]);
					break;
				}
			}
		}
		float rate = (fps <= 60 ? fps : std::sqrt(fps / 60.0f) * 60.0f) / 30.0f;
		return (int)std::lround(resolution * rate) * 1000;
	}

	// Whether the bitrate follows the resolution and the frame rate (« Auto »), per PC.
	String^ AutoKey(MoonlightHost^ host) { return L"autoBitrate-" + Uuid(host); }
	bool AutoBitrateOn(MoonlightHost^ host) {
		auto values = Windows::Storage::ApplicationData::Current->LocalSettings->Values;
		return !values->HasKey(AutoKey(host)) || safe_cast<bool>(values->Lookup(AutoKey(host)));
	}

	JsonObject^ StreamSettings(MoonlightHost^ host) {
		// The TV's own mode, from the HDMI output: its size and its fastest rate there.
		int screenWidth = 1920, screenHeight = 1080;
		double refresh = 60;
		try {
			auto hdmi = Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
			if (hdmi != nullptr) {
				auto current = hdmi->GetCurrentDisplayMode();
				if (current != nullptr) {
					screenWidth = current->ResolutionWidthInRawPixels;
					screenHeight = current->ResolutionHeightInRawPixels;
				}
				for (auto mode : hdmi->GetSupportedDisplayModes()) {
					if (mode->ResolutionWidthInRawPixels == (unsigned)screenWidth && mode->ResolutionHeightInRawPixels == (unsigned)screenHeight) refresh = std::max(refresh, mode->RefreshRate);
				}
			}
		}
		catch (...) {
		}
		auto text = [](String^ value) { return JsonValue::CreateStringValue(value); };
		auto flag = [](bool on) { return JsonValue::CreateStringValue(on ? L"true" : L"false"); };
		auto number = [](int value) { return JsonValue::CreateStringValue(ref new String(std::to_wstring(value).c_str())); };
		auto values = ref new JsonObject();
		auto width = host->Resolution != nullptr ? host->Resolution->Width : 1920;
		auto height = host->Resolution != nullptr ? host->Resolution->Height : 1080;
		values->Insert(L"list_resolution", text(ref new String((std::to_wstring(width) + L"x" + std::to_wstring(height)).c_str())));
		values->Insert(L"list_fps", number(host->FPS));
		values->Insert(L"seekbar_bitrate_kbps", AutoBitrateOn(host) ? text(L"auto") : number(host->Bitrate));
		values->Insert(L"video_format", text(host->VideoCodec == L"H.264" ? L"neverh265" : L"forceh265"));
		values->Insert(L"frame_pacing", text(host->FramePacing == L"Display-locked" ? L"smoothness" : L"latency"));
		values->Insert(L"checkbox_enable_hdr", flag(host->EnableHDR));
		values->Insert(L"checkbox_enable_perf_overlay", flag(host->EnableStats));
		values->Insert(L"list_audio_config", text(host->AudioConfig == L"Surround 7.1" ? L"71" : host->AudioConfig == L"Surround 5.1" ? L"51" : L"2"));
		values->Insert(L"checkbox_host_audio", flag(host->PlayAudioOnPC));
		values->Insert(L"checkbox_enable_sops", flag(host->EnableSOPS));
		auto settings = ref new JsonObject();
		settings->Insert(L"values", values);
		settings->Insert(L"screen", text(ref new String((std::to_wstring(screenWidth) + L"x" + std::to_wstring(screenHeight)).c_str())));
		settings->Insert(L"refreshRate", JsonValue::CreateNumberValue(std::round(refresh)));
		settings->Insert(L"autoBitrate", JsonValue::CreateNumberValue(AutoBitrate(width, height, host->FPS)));
		return settings;
	}

	void ApplyStreamSetting(MoonlightHost^ host, const std::wstring& key, String^ value) {
		std::wstring v(value->Data());
		bool on = v == L"true";
		if (key == L"list_resolution") {
			auto x = v.find(L'x');
			if (x == std::wstring::npos) throw std::invalid_argument("Bad resolution");
			host->Resolution = ref new ScreenResolution(std::stoi(v.substr(0, x)), std::stoi(v.substr(x + 1)));
		}
		else if (key == L"list_fps") host->FPS = std::stoi(v);
		else if (key == L"seekbar_bitrate_kbps") {
			Windows::Storage::ApplicationData::Current->LocalSettings->Values->Insert(AutoKey(host), v == L"auto");
			if (v != L"auto") host->Bitrate = std::stoi(v);
		}
		else if (key == L"video_format") host->VideoCodec = v == L"neverh265" ? L"H.264" : L"HEVC (H.265)";
		else if (key == L"frame_pacing") host->FramePacing = v == L"smoothness" ? L"Display-locked" : L"Immediate";
		else if (key == L"checkbox_enable_hdr") {
			host->EnableHDR = on;
			// H.264 has no HDR here: HEVC carries it.
			if (on && host->VideoCodec == L"H.264") host->VideoCodec = L"HEVC (H.265)";
		}
		else if (key == L"checkbox_enable_perf_overlay") host->EnableStats = on;
		else if (key == L"list_audio_config") host->AudioConfig = v == L"71" ? L"Surround 7.1" : v == L"51" ? L"Surround 5.1" : L"Stereo";
		else if (key == L"checkbox_host_audio") host->PlayAudioOnPC = on;
		else if (key == L"checkbox_enable_sops") host->EnableSOPS = on;
		else throw std::invalid_argument("Unknown setting");
		if (AutoBitrateOn(host) && host->Resolution != nullptr) host->Bitrate = AutoBitrate(host->Resolution->Width, host->Resolution->Height, host->FPS);
	}

	// libgamestream for one PC, outside Moonlight's own pages.
	struct Server {
		SERVER_DATA data{};
		std::string folder;
		explicit Server(MoonlightHost^ host) {
			std::string parsedHost;
			int port = 0;
			if (!Utils::ParseHostPort(Utils::PlatformStringToStdString(host->LastHostname), parsedHost, port)) throw std::runtime_error("Bad address");
			folder = Utils::PlatformStringToStdString(Windows::Storage::ApplicationData::Current->LocalFolder->Path) + "\\";
			if (gs_init(&data, const_cast<char*>(parsedHost.c_str()), static_cast<unsigned short>(port), folder.c_str(), 3, true) != GS_OK) {
				throw Unavailable(std::string("The PC does not answer: ") + (gs_error ? gs_error : ""));
			}
		}
	};

	// ---- Cartouche Studio's live preview ----

	// An IPv4 address of a home network (10/8, 172.16/12, 192.168/16): the PC the TV sits beside.
	bool PrivateIpv4(const std::wstring& host) {
		int parts[4];
		wchar_t tail = 0;
		if (swscanf_s(host.c_str(), L"%d.%d.%d.%d%lc", &parts[0], &parts[1], &parts[2], &parts[3], &tail, 1) != 4) return false;
		for (int part : parts)
			if (part < 0 || part > 255) return false;
		return parts[0] == 10 || (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) || (parts[0] == 192 && parts[1] == 168);
	}

	// "https://192.168.1.20:47976" of an address the preview may load: https, a home address, a port.
	String^ PreviewOriginOf(String^ address) {
		Uri^ uri;
		try {
			uri = ref new Uri(address);
		}
		catch (...) {
			throw std::runtime_error("Adresse d'aperçu invalide");
		}
		if (uri->SchemeName != L"https" || !PrivateIpv4(uri->Host->Data()) || uri->Port <= 0) throw std::runtime_error("L'aperçu vient d'un PC de ce réseau, en https");
		return L"https://" + uri->Host + L":" + uri->Port.ToString();
	}

	bool SameOrigin(String^ address, String^ origin) {
		if (address == nullptr || origin == nullptr) return false;
		try {
			auto uri = ref new Uri(address);
			return (L"https://" + uri->Host + L":" + uri->Port.ToString()) == origin && uri->SchemeName == L"https";
		}
		catch (...) {
			return false;
		}
	}

	// SHA-256 of a PEM certificate, hex in capitals: what the PC's lan-preview.ts gives as its fingerprint.
	String^ Fingerprint(String^ pem) {
		std::wstring text(pem->Data());
		std::wstring body;
		std::wistringstream lines(text);
		std::wstring line;
		while (std::getline(lines, line)) {
			if (line.find(L"-----") != std::wstring::npos) continue;
			for (wchar_t c : line)
				if (!std::iswspace(c)) body += c;
		}
		auto der = CryptographicBuffer::DecodeFromBase64String(ref new String(body.c_str()));
		auto sha = Windows::Security::Cryptography::Core::HashAlgorithmProvider::OpenAlgorithm(Windows::Security::Cryptography::Core::HashAlgorithmNames::Sha256);
		std::wstring hex(CryptographicBuffer::EncodeToHexString(sha->HashData(der))->Data());
		for (auto& c : hex) c = static_cast<wchar_t>(std::towupper(c));
		return ref new String(hex.c_str());
	}

	// In the preview's documents: the controller is the game's only while it is played (« Jouer »);
	// meanwhile it drives the Studio, and the game must not hear those presses.
	const wchar_t* PREVIEW_GATE = LR"JS((() => {
  try { navigator.gamepadInputEmulation = "gamepad"; } catch (e) {}
  const real = navigator.getGamepads ? navigator.getGamepads.bind(navigator) : () => [];
  Object.defineProperty(navigator, "getGamepads", { configurable: true, value: () => (window.__cartouchePlaying ? real() : [null, null, null, null]) });
})();)JS";
}

CartouchePage::CartouchePage() {
	InitializeComponent();
	state = GetApplicationState();
	// A stream goes back here: the page, its interface and its state stay as they were.
	NavigationCacheMode = Windows::UI::Xaml::Navigation::NavigationCacheMode::Required;
	discoveredDevices = ref new Platform::Collections::Map<String^, DeviceInformation^>();
	probing = std::make_shared<std::set<std::string>>();
	deviceWatcherHelper = ref new DeviceWatcherHelper(Dispatcher);
	WeakReference weakThis(this);
	deviceWatcherHelper->DeviceAdded += ref new TypedEventHandler<DeviceWatcher^, DeviceInformation^>([weakThis](DeviceWatcher^ sender, DeviceInformation^ info) {
		auto page = weakThis.Resolve<CartouchePage>();
		if (page != nullptr) page->OnDeviceAdded(sender, info);
	});
	deviceWatcherHelper->DeviceUpdated += ref new TypedEventHandler<DeviceWatcher^, DeviceInformationUpdate^>([weakThis](DeviceWatcher^ sender, DeviceInformationUpdate^ info) {
		auto page = weakThis.Resolve<CartouchePage>();
		if (page != nullptr) page->OnDeviceUpdated(sender, info);
	});
}

void CartouchePage::OnNavigatedTo(NavigationEventArgs^ e) {
	Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseVisible);
	backToken = SystemNavigationManager::GetForCurrentView()->BackRequested += ref new EventHandler<BackRequestedEventArgs^>(this, &CartouchePage::OnBackRequested);
	Web->Visibility = Windows::UI::Xaml::Visibility::Visible;
	if (Preview->CoreWebView2 != nullptr) Preview->CoreWebView2->Resume();
	if (Web->CoreWebView2 != nullptr) {
		Web->CoreWebView2->Resume();
		// Back from a stream or Moonlight's pages: the controller is the page's again.
		Web->Focus(Windows::UI::Xaml::FocusState::Programmatic);
	}
	Start();
	StartDiscovery();

	// The PCs' state while Cartouche shows: online, paired, the game running.
	if (!polling.exchange(true)) {
		WeakReference weakThis(this);
		create_task([weakThis]() {
			while (true) {
				Sleep(5000);
				auto page = weakThis.Resolve<CartouchePage>();
				if (page == nullptr || !page->polling.load()) break;
				std::vector<MoonlightHost^> hosts;
				for (auto host : page->state->SavedHosts) hosts.push_back(host);
				page = nullptr;
				for (auto host : hosts) {
					try {
						host->UpdateHostInfo(false);
					}
					catch (...) {
					}
				}
			}
		});
	}
}

void CartouchePage::OnNavigatedFrom(NavigationEventArgs^ e) {
	SystemNavigationManager::GetForCurrentView()->BackRequested -= backToken;
	polling.store(false);
	deviceWatcherHelper->Reset();
	discoveredDevices->Clear();
	// While a game streams, the interface sleeps (memory and GPU go to the stream), the Studio's preview with it.
	Web->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	if (previewPlaying.load()) PreviewPlaying(false);
	Preview->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	if (Preview->CoreWebView2 != nullptr) {
		create_task(Preview->CoreWebView2->TrySuspendAsync()).then([](task<bool> t) {
			try {
				t.get();
			}
			catch (...) {
			}
		});
	}
	if (Web->CoreWebView2 != nullptr) {
		create_task(Web->CoreWebView2->TrySuspendAsync()).then([](task<bool> t) {
			try {
				t.get();
			}
			catch (...) {
			}
		});
	}
}

void CartouchePage::OnBackRequested(Object^ sender, BackRequestedEventArgs^ args) {
	// B is the page's (Gamepad API, input.ts): unhandled, Xbox would suspend the app.
	args->Handled = true;
}

void CartouchePage::OnStateLoaded() {
	for (auto host : state->SavedHosts) {
		Watch(host);
		auto hostname = host->LastHostname;
		create_task([host, hostname]() {
			try {
				GetApplicationState()->AddHost(hostname);
			}
			catch (...) {
			}
			Windows::ApplicationModel::Core::CoreApplication::MainView->CoreWindow->Dispatcher->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([host]() { host->Loading = false; }));
		});
	}
	WeakReference weakThis(this);
	auto observable = dynamic_cast<Windows::Foundation::Collections::IObservableVector<MoonlightHost^>^>(state->SavedHosts);
	if (observable == nullptr) return;
	observable->VectorChanged += ref new Windows::Foundation::Collections::VectorChangedEventHandler<MoonlightHost^>(
		[weakThis](Windows::Foundation::Collections::IObservableVector<MoonlightHost^>^ hosts, Windows::Foundation::Collections::IVectorChangedEventArgs^ change) {
			auto page = weakThis.Resolve<CartouchePage>();
			if (page == nullptr) return;
			for (auto host : hosts) page->Watch(host);
		});
}

void CartouchePage::Start() {
	if (started) return;
	started = true;
	Intro->Begin();
	Waiting->Begin();
	WatchReady();
	// The interface's sounds (and the browser games') play from the controller: on a console no
	// press is a "user gesture", so the WebView may play sound without one. Read when the
	// WebView's browser starts, before the first EnsureCoreWebView2Async.
	SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"--autoplay-policy=no-user-gesture-required");
	WeakReference weakThis(this);
	create_task(Web->EnsureCoreWebView2Async()).then([weakThis](task<void> t) {
		try {
			t.get();
		}
		catch (Exception^ e) {
			Utils::Logf("[Cartouche] WebView2 failed: %s\n", Utils::PlatformStringToStdString(e->Message).c_str());
			return;
		}
		auto page = weakThis.Resolve<CartouchePage>();
		if (page == nullptr) return;
		auto core = page->Web->CoreWebView2;
		core->Settings->IsStatusBarEnabled = false;
		core->Settings->AreDefaultContextMenusEnabled = false;
		core->Settings->IsZoomControlEnabled = false;
#if !defined(_DEBUG)
		core->Settings->AreDevToolsEnabled = false;
#endif
		core->SetVirtualHostNameToFolderMapping(ref new String(COVERS_HOST), ref new String(LocalFolder(L"covers").c_str()), CoreWebView2HostResourceAccessKind::Allow);
		core->SetVirtualHostNameToFolderMapping(ref new String(PC_COVERS_HOST), ref new String(LocalFolder(L"pc-covers").c_str()), CoreWebView2HostResourceAccessKind::Allow);
		core->WebMessageReceived += ref new TypedEventHandler<CoreWebView2^, CoreWebView2WebMessageReceivedEventArgs^>(page, &CartouchePage::OnWebMessage);
		core->NavigationStarting += ref new TypedEventHandler<CoreWebView2^, CoreWebView2NavigationStartingEventArgs^>(page, &CartouchePage::OnNavigationStarting);
		core->NewWindowRequested += ref new TypedEventHandler<CoreWebView2^, CoreWebView2NewWindowRequestedEventArgs^>(page, &CartouchePage::OnNewWindow);
		// The page reads the controller only while its document has the focus (input.ts): the
		// WebView takes it as soon as a page has loaded, and again if XAML ever moves it away.
		core->NavigationCompleted += ref new TypedEventHandler<CoreWebView2^, CoreWebView2NavigationCompletedEventArgs^>([weakThis](CoreWebView2^ sender, CoreWebView2NavigationCompletedEventArgs^ args) {
			auto inner = weakThis.Resolve<CartouchePage>();
			if (inner != nullptr) inner->Web->Focus(Windows::UI::Xaml::FocusState::Programmatic);
		});
		page->Web->LostFocus += ref new RoutedEventHandler([weakThis](Object^ sender, RoutedEventArgs^ args) {
			auto inner = weakThis.Resolve<CartouchePage>();
			if (inner == nullptr || inner->Frame == nullptr || inner->Frame->Content != inner || inner->previewPlaying.load()) return;
			inner->Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([weakThis]() {
				auto again = weakThis.Resolve<CartouchePage>();
				if (again != nullptr && again->Frame != nullptr && again->Frame->Content == again && !again->previewPlaying.load()) again->Web->Focus(Windows::UI::Xaml::FocusState::Programmatic);
			}));
		});
		page->Web->Focus(Windows::UI::Xaml::FocusState::Programmatic);
		create_task(core->AddScriptToExecuteOnDocumentCreatedAsync(ref new String(CHANNEL))).then([weakThis](task<String^> added) {
			try {
				added.get();
			}
			catch (...) {
			}
			auto inner = weakThis.Resolve<CartouchePage>();
			if (inner != nullptr) inner->Web->CoreWebView2->Navigate(ref new String(HOME));
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

// Drawn: the interface is up (main.tsx adds has-app, then its top bar once the catalog is in)
// and the images on screen have arrived, so it appears whole, not tile by tile; 2 s after that at most (a screen without
// images, lazy covers the WebView does not load while it is covered).
static const wchar_t* READY = LR"JS((() => {
  // The interface itself (its top bar), not its boot cartridge while the catalog loads.
  if (!document.documentElement.classList.contains("has-app") || !document.querySelector(".topbar")) return false;
  window.__cartoucheMounted ??= performance.now();
  const shown = [...document.images].filter((image) => { const box = image.getBoundingClientRect(); return box.width > 0 && box.bottom > 0 && box.top < innerHeight; });
  return (shown.length > 0 && shown.every((image) => image.complete)) || performance.now() - window.__cartoucheMounted > 2000;
})())JS";

void CartouchePage::WatchReady() {
	shownAt = GetTickCount64();
	readyTimer = ref new DispatcherTimer();
	TimeSpan every;
	every.Duration = 200 * 10000;
	readyTimer->Interval = every;
	WeakReference weakThis(this);
	readyTimer->Tick += ref new EventHandler<Object^>([weakThis](Object^ sender, Object^ args) {
		auto page = weakThis.Resolve<CartouchePage>();
		if (page == nullptr || page->revealed) return;
		auto elapsed = GetTickCount64() - page->shownAt;
		// Long enough for the intro to land; never longer than 15 s (no network: the page says so).
		if (elapsed > 15000) return page->Reveal();
		if (page->checking || page->Web->CoreWebView2 == nullptr) return;
		page->checking = true;
		create_task(page->Web->CoreWebView2->ExecuteScriptAsync(ref new String(READY))).then([weakThis](task<String^> done) {
			auto inner = weakThis.Resolve<CartouchePage>();
			if (inner == nullptr) return;
			inner->checking = false;
			String^ result = nullptr;
			try {
				result = done.get();
			}
			catch (...) {
			}
			if (result == L"true" && GetTickCount64() - inner->shownAt >= 1500) inner->Reveal();
		}, task_continuation_context::use_current());
	});
	readyTimer->Start();
}

void CartouchePage::Reveal() {
	if (revealed) return;
	revealed = true;
	readyTimer->Stop();
	WeakReference weakThis(this);
	Outro->Completed += ref new EventHandler<Object^>([weakThis](Object^ sender, Object^ args) {
		auto page = weakThis.Resolve<CartouchePage>();
		if (page == nullptr) return;
		page->Loader->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		page->Waiting->Stop();
	});
	Outro->Begin();
	Web->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void CartouchePage::OnNavigationStarting(CoreWebView2^ sender, CoreWebView2NavigationStartingEventArgs^ args) {
	// The window stays Cartouche's: any other page opens in Edge.
	if (Ours(args->Uri)) return;
	args->Cancel = true;
	std::wstring url(args->Uri->Data());
	if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0) Launcher::LaunchUriAsync(ref new Uri(args->Uri));
}

void CartouchePage::OnNewWindow(CoreWebView2^ sender, CoreWebView2NewWindowRequestedEventArgs^ args) {
	args->Handled = true;
	std::wstring url(args->Uri->Data());
	if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0) Launcher::LaunchUriAsync(ref new Uri(args->Uri));
}

// ---- The channel: {id, method, args} in, {id, ok, value | error} or {event, data} out ----

void CartouchePage::OnWebMessage(CoreWebView2^ sender, CoreWebView2WebMessageReceivedEventArgs^ args) {
	if (!Ours(args->Source)) return;
	String^ text;
	try {
		text = args->TryGetWebMessageAsString();
	}
	catch (...) {
		return;
	}
	JsonObject^ message;
	if (!JsonObject::TryParse(text, &message)) return;
	int id = static_cast<int>(message->GetNamedNumber(L"id", 0));
	String^ method = message->GetNamedString(L"method", L"");
	JsonArray^ callArgs = message->HasKey(L"args") ? message->GetNamedArray(L"args", ref new JsonArray()) : ref new JsonArray();
	WeakReference weakThis(this);
	task<IJsonValue^> work;
	try {
		work = Call(method, callArgs);
	}
	catch (Exception^ e) {
		return Fail(id, e->Message);
	}
	catch (const std::exception& e) {
		return Fail(id, Wide(e.what()));
	}
	work.then([weakThis, id](task<IJsonValue^> done) {
		auto page = weakThis.Resolve<CartouchePage>();
		if (page == nullptr) return;
		try {
			page->Answer(id, done.get());
		}
		catch (Exception^ e) {
			page->Fail(id, e->Message);
		}
		catch (const std::exception& e) {
			page->Fail(id, Wide(e.what()));
		}
		catch (...) {
			page->Fail(id, L"Erreur Xbox");
		}
	});
}

void CartouchePage::Answer(int id, IJsonValue^ value) {
	auto reply = ref new JsonObject();
	reply->Insert(L"id", JsonValue::CreateNumberValue(id));
	reply->Insert(L"ok", JsonValue::CreateBooleanValue(true));
	reply->Insert(L"value", value != nullptr ? value : JsonValue::CreateNullValue());
	Post(reply);
}

void CartouchePage::Fail(int id, String^ error) {
	auto reply = ref new JsonObject();
	reply->Insert(L"id", JsonValue::CreateNumberValue(id));
	reply->Insert(L"ok", JsonValue::CreateBooleanValue(false));
	reply->Insert(L"error", JsonValue::CreateStringValue(error));
	Post(reply);
}

void CartouchePage::Event(String^ name, IJsonValue^ data) {
	auto message = ref new JsonObject();
	message->Insert(L"event", JsonValue::CreateStringValue(name));
	message->Insert(L"data", data);
	Post(message);
}

void CartouchePage::Post(JsonObject^ message) {
	auto text = message->Stringify();
	WeakReference weakThis(this);
	auto send = ref new DispatchedHandler([weakThis, text]() {
		auto page = weakThis.Resolve<CartouchePage>();
		if (page == nullptr || page->Web->CoreWebView2 == nullptr) return;
		// Only to the site's documents (a page elsewhere never hears the app).
		if (!Ours(page->Web->CoreWebView2->Source)) return;
		page->Web->CoreWebView2->PostWebMessageAsString(text);
	});
	if (Dispatcher->HasThreadAccess) send();
	else Dispatcher->RunAsync(CoreDispatcherPriority::Normal, send);
}

task<IJsonValue^> CartouchePage::Call(String^ method, JsonArray^ args) {
	std::wstring name(method->Data());
	auto value = [](IJsonValue^ v) { return task_from_result(v); };

	// Cartouche Studio's live preview: its own calls (PreviewCall).
	if (name.rfind(L"preview", 0) == 0) return PreviewCall(name, args);
	if (name == L"hello") {
		auto hello = ref new JsonObject();
		auto version = Windows::ApplicationModel::Package::Current->Id->Version;
		hello->Insert(L"version", JsonValue::CreateStringValue(version.Major + L"." + version.Minor + L"." + version.Build));
		hello->Insert(L"computers", Computers());
		// What this app can do beyond Android's calls: the page asks before it uses one.
		auto features = ref new JsonArray();
		features->Append(JsonValue::CreateStringValue(L"preview"));
		hello->Insert(L"features", features);
		return value(hello);
	}
	if (name == L"computers") return value(Computers());
	if (name == L"addComputer") {
		String^ address = args->GetStringAt(0);
		return create_task([address]() -> IJsonValue^ {
			std::wstring trimmed(address->Data());
			trimmed.erase(0, trimmed.find_first_not_of(L" \t"));
			trimmed.erase(trimmed.find_last_not_of(L" \t") + 1);
			return JsonValue::CreateBooleanValue(GetApplicationState()->AddHost(ref new String(trimmed.c_str())));
		});
	}
	if (name == L"pair") {
		MoonlightHost^ host = Host(args->GetStringAt(0));
		WeakReference weakThis(this);
		// The PIN first (typed in Cartouche on the PC); the outcome follows as a "paired" event.
		return create_task([host, weakThis]() -> IJsonValue^ {
			auto client = new MoonlightClient();
			char address[2048];
			wcstombs_s(NULL, address, host->LastHostname->Data(), 2047);
			if (client->Connect(address) != 0) {
				delete client;
				throw std::runtime_error("Le PC ne répond pas");
			}
			std::string pin(client->GeneratePIN());
			auto uuid = Uuid(host);
			create_task([client, host, weakThis, uuid]() {
				int status = client->Pair();
				std::string error = status == 0 ? "" : (gs_error ? gs_error : "pairing failed");
				delete client;
				try {
					host->UpdateHostInfo(false);
				}
				catch (...) {
				}
				auto page = weakThis.Resolve<CartouchePage>();
				if (page == nullptr) return;
				auto outcome = ref new JsonObject();
				outcome->Insert(L"uuid", JsonValue::CreateStringValue(uuid));
				outcome->Insert(L"ok", JsonValue::CreateBooleanValue(status == 0));
				outcome->Insert(L"error", status == 0 ? JsonValue::CreateNullValue() : JsonValue::CreateStringValue(Wide(error)));
				page->Event(L"paired", outcome);
			});
			auto started = ref new JsonObject();
			started->Insert(L"pin", JsonValue::CreateStringValue(Wide(pin)));
			return started;
		});
	}
	if (name == L"apps") {
		MoonlightHost^ host = Host(args->GetStringAt(0));
		// The PC's apps (Cartouche Stream publishes Ma collection there), each with its cover, fetched
		// once and kept.
		return create_task([host]() -> IJsonValue^ {
			Server server(host);
			PAPP_LIST list = nullptr;
			if (gs_applist(&server.data, &list) != GS_OK) throw std::runtime_error(gs_error ? gs_error : "No games");
			LocalFolder(L"covers");
			auto folder = LocalFolder(L"covers\\" + std::wstring(Uuid(host)->Data()));
			std::string folderA = Utils::PlatformStringToStdString(ref new String(folder.c_str()));
			auto apps = ref new JsonArray();
			std::wstring uuid(Uuid(host)->Data());
			for (PAPP_LIST app = list; app != nullptr; app = app->next) {
				std::wstring cover = std::to_wstring(app->id) + L".png";
				if (GetFileAttributesW((folder + cover).c_str()) == INVALID_FILE_ATTRIBUTES) gs_appasset(&server.data, folderA.c_str(), app->id);
				auto json = ref new JsonObject();
				json->Insert(L"id", JsonValue::CreateNumberValue(app->id));
				json->Insert(L"name", JsonValue::CreateStringValue(Wide(app->name ? app->name : "")));
				json->Insert(L"hdr", JsonValue::CreateBooleanValue(false));
				json->Insert(L"cover", Served(COVERS_HOST, folder + cover, uuid + L"/" + cover));
				apps->Append(json);
			}
			while (list != nullptr) {
				PAPP_LIST next = list->next;
				free(list->name);
				free(list);
				list = next;
			}
			return apps;
		});
	}
	if (name == L"play") {
		MoonlightHost^ host = Host(args->GetStringAt(0));
		int appId = static_cast<int>(args->GetNumberAt(1));
		String^ appName = args->GetStringAt(2);
		WeakReference weakThis(this);
		// Another game running on the PC: it is closed first (Moonlight asks; the page already did).
		return create_task([host, appId]() {
			try {
				host->UpdateHostInfo(false);
				if (host->CurrentlyRunningAppId != 0 && host->CurrentlyRunningAppId != appId) {
					Server server(host);
					gs_quit_app(&server.data);
				}
			}
			catch (...) {
			}
		}).then([weakThis, host, appId, appName]() -> IJsonValue^ {
			auto page = weakThis.Resolve<CartouchePage>();
			if (page != nullptr) page->Play(host, appId, appName);
			return JsonValue::CreateNullValue();
		}, task_continuation_context::use_current());
	}
	if (name == L"quit") {
		MoonlightHost^ host = Host(args->GetStringAt(0));
		return create_task([host]() -> IJsonValue^ {
			Server server(host);
			bool ok = gs_quit_app(&server.data) == GS_OK;
			try {
				host->UpdateHostInfo(false);
			}
			catch (...) {
			}
			return JsonValue::CreateBooleanValue(ok);
		});
	}
	if (name == L"playWeb") {
		String^ url = args->GetStringAt(0);
		std::wstring target(url->Data());
		if (target.rfind(L"https://", 0) != 0 && target.rfind(L"http://", 0) != 0) throw std::invalid_argument("Not a web game");
		Frame->Navigate(Windows::UI::Xaml::Interop::TypeName(WebGamePage::typeid), url);
		return value(JsonValue::CreateNullValue());
	}
	if (name == L"openBrowser") {
		String^ url = args->GetStringAt(0);
		if (!Ours(url)) throw std::invalid_argument("Not a page of Cartouche");
		Launcher::LaunchUriAsync(ref new Uri(url));
		return value(JsonValue::CreateNullValue());
	}
	if (name == L"settings") {
		// Moonlight's own screen for the PC: the rare settings (Réglages avancés).
		Frame->Navigate(Windows::UI::Xaml::Interop::TypeName(HostSettingsPage::typeid), SettingsHost(state));
		return value(JsonValue::CreateNullValue());
	}
	if (name == L"streamSettings") return value(StreamSettings(SettingsHost(state)));
	if (name == L"setStreamSetting") {
		auto host = SettingsHost(state);
		ApplyStreamSetting(host, std::wstring(args->GetStringAt(0)->Data()), args->GetStringAt(1));
		state->UpdateFile();
		return value(StreamSettings(host));
	}
	if (name == L"moonlight") {
		Frame->Navigate(Windows::UI::Xaml::Interop::TypeName(HostSelectorPage::typeid));
		return value(JsonValue::CreateNullValue());
	}
	if (name == L"pcLibrary") {
		MoonlightHost^ host = Host(args->GetStringAt(0));
		// Null when no Cartouche answers there: the page shows the stream server's list.
		return create_task([host]() -> IJsonValue^ {
			try {
				auto library = ApiJson(host, "/v1/hello");
				library->Insert(L"games", ApiJson(host, "/v1/games")->GetNamedArray(L"games"));
				return library;
			}
			catch (const Unavailable&) {
				return JsonValue::CreateNullValue();
			}
		});
	}
	if (name == L"pcGame") {
		MoonlightHost^ host = Host(args->GetStringAt(0));
		std::string id = GameId(args->GetStringAt(1));
		return create_task([host, id]() -> IJsonValue^ { return ApiJson(host, "/v1/games/" + id); });
	}
	if (name == L"pcCover") {
		MoonlightHost^ host = Host(args->GetStringAt(0));
		std::string id = GameId(args->GetStringAt(1));
		std::wstring fingerprint(args->Size > 2 ? args->GetStringAt(2)->Data() : L"");
		fingerprint.erase(std::remove_if(fingerprint.begin(), fingerprint.end(), [](wchar_t c) { return !iswalnum(c); }), fingerprint.end());
		// Kept under its fingerprint: a new cover is a new file.
		return create_task([host, id, fingerprint]() -> IJsonValue^ {
			std::wstring name = std::wstring(Uuid(host)->Data()) + L"-" + std::wstring(id.begin(), id.end()) + L"-" + fingerprint + L".png";
			std::wstring file = LocalFolder(L"pc-covers") + name;
			if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) WriteAll(file, ApiGet(host, "/v1/games/" + id + "/cover"));
			return Served(PC_COVERS_HOST, file, name);
		});
	}
	throw std::invalid_argument("Unknown method " + Utils::PlatformStringToStdString(method));
}

MoonlightHost^ CartouchePage::Host(String^ uuid) {
	for (auto host : state->SavedHosts) {
		if (Uuid(host) == uuid) return host;
	}
	throw std::runtime_error("Unknown PC");
}

JsonArray^ CartouchePage::Computers() {
	auto list = ref new JsonArray();
	for (auto host : state->SavedHosts) {
		auto json = ref new JsonObject();
		json->Insert(L"uuid", JsonValue::CreateStringValue(Uuid(host)));
		json->Insert(L"name", JsonValue::CreateStringValue(host->ComputerName != nullptr && !host->ComputerName->IsEmpty() ? host->ComputerName : host->LastHostname));
		json->Insert(L"state", JsonValue::CreateStringValue(host->Loading ? L"unknown" : host->Connected ? L"online" : L"offline"));
		json->Insert(L"paired", JsonValue::CreateBooleanValue(host->Paired));
		json->Insert(L"address", JsonValue::CreateStringValue(Wide(HostPart(host->LastHostname))));
		json->Insert(L"runningGameId", JsonValue::CreateNumberValue(host->CurrentlyRunningAppId));
		list->Append(json);
	}
	return list;
}

// A PC's state changes: the page hears it as a "computer" event.
void CartouchePage::Watch(MoonlightHost^ host) {
	if (std::find(watched.begin(), watched.end(), host) != watched.end()) return;
	watched.push_back(host);
	WeakReference weakThis(this);
	host->PropertyChanged += ref new Windows::UI::Xaml::Data::PropertyChangedEventHandler([weakThis, host](Object^ sender, Windows::UI::Xaml::Data::PropertyChangedEventArgs^ change) {
		std::wstring property(change->PropertyName->Data());
		if (property != L"Connected" && property != L"Paired" && property != L"Loading" && property != L"ComputerName" && property != L"CurrentlyRunningAppId") return;
		auto page = weakThis.Resolve<CartouchePage>();
		if (page == nullptr) return;
		auto computers = page->Computers();
		for (auto entry : computers) {
			auto json = entry->GetObject();
			if (json->GetNamedString(L"uuid") == Uuid(host)) page->Event(L"computer", json);
		}
	});
}

void CartouchePage::Play(MoonlightHost^ host, int appId, String^ name) {
	// As Moonlight's AppPage does, with the PC's own settings (HostSettingsPage).
	StreamConfiguration^ config = ref new StreamConfiguration();
	config->hostname = host->LastHostname;
	config->appID = appId;
	config->appName = name;
	// Its cover for the launch screen: the one Cartouche drew for the PC's list (apps).
	std::wstring cover = LocalFolder(L"covers\\" + std::wstring(Uuid(host)->Data())) + std::to_wstring(appId) + L".png";
	if (GetFileAttributesW(cover.c_str()) != INVALID_FILE_ATTRIBUTES) config->coverUri = L"ms-appdata:///local/covers/" + Uuid(host) + L"/" + ref new String(std::to_wstring(appId).c_str()) + L".png";
	config->computerName = host->ComputerName;
	config->width = host->Resolution->Width;
	config->height = host->Resolution->Height;
	config->bitrate = host->Bitrate;
	config->FPS = host->FPS;
	config->audioConfig = host->AudioConfig;
	config->videoCodec = host->VideoCodec;
	config->playAudioOnPC = host->PlayAudioOnPC;
	config->enableHDR = host->EnableHDR;
	config->enableSOPS = host->EnableSOPS;
	config->framePacing = host->FramePacing;
	config->audioBuffer = host->AudioBuffer;
	config->packetSize = host->PacketSize;
	config->enableStats = host->EnableStats;
	config->enableGraphs = host->EnableGraphs;
	if (config->enableHDR && config->videoCodec == "H.264") config->videoCodec = "HEVC (H.265)";
	if (IsXbox() && config->videoCodec == "H.264") {
		// The hardware H.264 decoder of both Xbox generations stops at 1080p60.
		if (config->width > 1920) {
			config->width = 1920;
			config->height = 1080;
		}
		if (config->FPS > 60) config->FPS = 60;
	}
	Frame->Navigate(Windows::UI::Xaml::Interop::TypeName(StreamPage::typeid), config);
}

// ---- Discovery: the PCs that announce Moonlight's service on the network ----

void CartouchePage::StartDiscovery() {
	String^ selector =
		L"System.Devices.AepService.ProtocolId:=\"{4526e8c1-8aac-4153-9b16-55e86ada0e54}\""
		L" AND System.Devices.Dnssd.Domain:=\"local\""
		L" AND System.Devices.Dnssd.ServiceName:=\"_nvstream._tcp\"";
	auto props = ref new Platform::Collections::Vector<String^>({
		L"System.Devices.Dnssd.HostName",
		L"System.Devices.Dnssd.PortNumber",
		L"System.Devices.IpAddress",
	});
	discoveredDevices->Clear();
	probing = std::make_shared<std::set<std::string>>();
	deviceWatcherHelper->StartWatcher(DeviceInformation::CreateWatcher(selector, props, DeviceInformationKind::AssociationEndpointService));
}

void CartouchePage::OnDeviceAdded(DeviceWatcher^ sender, DeviceInformation^ info) {
	discoveredDevices->Insert(info->Id, info);
	TryAddDiscoveredHost(info);
}

void CartouchePage::OnDeviceUpdated(DeviceWatcher^ sender, DeviceInformationUpdate^ info) {
	if (!discoveredDevices->HasKey(info->Id)) return;
	auto device = discoveredDevices->Lookup(info->Id);
	device->Update(info);
	TryAddDiscoveredHost(device);
}

void CartouchePage::TryAddDiscoveredHost(DeviceInformation^ info) {
	auto properties = info->Properties;
	if (!properties->HasKey(L"System.Devices.IpAddress") || !properties->HasKey(L"System.Devices.Dnssd.PortNumber")) return;
	auto addressValue = dynamic_cast<IPropertyValue^>(properties->Lookup(L"System.Devices.IpAddress"));
	auto portValue = dynamic_cast<IPropertyValue^>(properties->Lookup(L"System.Devices.Dnssd.PortNumber"));
	if (addressValue == nullptr || addressValue->Type != PropertyType::StringArray || portValue == nullptr || portValue->Type != PropertyType::UInt16) return;
	auto port = portValue->GetUInt16();
	if (port == 0) return;
	Platform::Array<String^>^ addresses;
	addressValue->GetStringArray(&addresses);
	String^ endpoint = nullptr;
	for (auto address : addresses) {
		IN_ADDR ipv4;
		// IPv4 only here: the PC and the Xbox share the home network.
		if (address != nullptr && InetPtonW(AF_INET, address->Data(), &ipv4) == 1) {
			endpoint = address + L":" + ref new String(std::to_wstring(port).c_str());
			break;
		}
	}
	if (endpoint == nullptr) return;
	auto key = Utils::PlatformStringToStdString(endpoint);
	auto inFlight = probing;
	if (!inFlight->insert(key).second) return;
	create_task([endpoint, key, inFlight]() {
		try {
			GetApplicationState()->AddHost(endpoint);
		}
		catch (...) {
		}
	}).then([key, inFlight]() { inFlight->erase(key); }, task_continuation_context::use_current());
}

// ---- Cartouche Studio's live preview: the page's stage, filled by the game the PC serves ----

void CartouchePage::PreviewEvent(String^ kind, String^ detail) {
	auto data = ref new JsonObject();
	data->Insert(L"dock", JsonValue::CreateStringValue(L"xbox-preview"));
	data->Insert(L"kind", JsonValue::CreateStringValue(kind));
	if (kind == L"load") {
		data->Insert(L"url", JsonValue::CreateStringValue(Preview->CoreWebView2 != nullptr ? Preview->CoreWebView2->Source : L""));
		data->Insert(L"perf", JsonValue::CreateBooleanValue(false));
	}
	if (detail != nullptr) data->Insert(L"detail", JsonValue::CreateStringValue(detail));
	Event(L"preview", data);
}

void CartouchePage::WirePreview() {
	if (previewWired) return;
	previewWired = true;
	auto core = Preview->CoreWebView2;
	core->Settings->IsStatusBarEnabled = false;
	core->Settings->AreDefaultContextMenusEnabled = false;
	core->Settings->IsZoomControlEnabled = false;
#if !defined(_DEBUG)
	core->Settings->AreDevToolsEnabled = false;
#endif
	WeakReference weakThis(this);
	// The PC's own certificate, and only it: the fingerprint the page got through the account.
	core->ServerCertificateErrorDetected += ref new TypedEventHandler<CoreWebView2^, CoreWebView2ServerCertificateErrorDetectedEventArgs^>([weakThis](CoreWebView2^ sender, CoreWebView2ServerCertificateErrorDetectedEventArgs^ args) {
		auto page = weakThis.Resolve<CartouchePage>();
		bool pinned = false;
		try {
			pinned = page != nullptr && page->previewFingerprint != nullptr && SameOrigin(args->RequestUri, page->previewOrigin) && Fingerprint(args->ServerCertificate->ToPemEncoding()) == page->previewFingerprint;
		}
		catch (...) {
		}
		args->Action = pinned ? CoreWebView2ServerCertificateErrorAction::AlwaysAllow : CoreWebView2ServerCertificateErrorAction::Cancel;
	});
	// It stays on the PC's preview: anything else is refused.
	core->NavigationStarting += ref new TypedEventHandler<CoreWebView2^, CoreWebView2NavigationStartingEventArgs^>([weakThis](CoreWebView2^ sender, CoreWebView2NavigationStartingEventArgs^ args) {
		auto page = weakThis.Resolve<CartouchePage>();
		if (args->Uri == L"about:blank") return;
		if (page == nullptr || !SameOrigin(args->Uri, page->previewOrigin)) args->Cancel = true;
	});
	core->NewWindowRequested += ref new TypedEventHandler<CoreWebView2^, CoreWebView2NewWindowRequestedEventArgs^>([](CoreWebView2^ sender, CoreWebView2NewWindowRequestedEventArgs^ args) { args->Handled = true; });
	core->NavigationCompleted += ref new TypedEventHandler<CoreWebView2^, CoreWebView2NavigationCompletedEventArgs^>([weakThis](CoreWebView2^ sender, CoreWebView2NavigationCompletedEventArgs^ args) {
		auto page = weakThis.Resolve<CartouchePage>();
		if (page == nullptr || sender->Source == L"about:blank") return;
		page->previewLoaded = args->IsSuccess;
		// Played across a reload (the agent's change): the game keeps the controller.
		if (page->previewPlaying.load()) sender->ExecuteScriptAsync(L"window.__cartouchePlaying = true");
		page->PreviewEvent(args->IsSuccess ? L"load" : L"failed", args->IsSuccess ? nullptr : args->WebErrorStatus.ToString());
	});
	// The answers of the page's calls into the game (previewEval), back to the page as events.
	core->WebMessageReceived += ref new TypedEventHandler<CoreWebView2^, CoreWebView2WebMessageReceivedEventArgs^>([weakThis](CoreWebView2^ sender, CoreWebView2WebMessageReceivedEventArgs^ args) {
		auto page = weakThis.Resolve<CartouchePage>();
		if (page == nullptr || !SameOrigin(args->Source, page->previewOrigin)) return;
		String^ text;
		try {
			text = args->TryGetWebMessageAsString();
		}
		catch (...) {
			return;
		}
		JsonObject^ answer;
		if (text == nullptr || text->Length() > 4 * 1024 * 1024 || !JsonObject::TryParse(text, &answer) || !answer->HasKey(L"cartouchePreview")) return;
		page->Event(L"previewResult", answer);
	});
	core->AddScriptToExecuteOnDocumentCreatedAsync(ref new String(PREVIEW_GATE));
}

void CartouchePage::PreviewPlaying(bool on) {
	if (previewPlaying.exchange(on) == on) return;
	if (Preview->CoreWebView2 != nullptr) Preview->CoreWebView2->ExecuteScriptAsync(on ? L"window.__cartouchePlaying = true" : L"window.__cartouchePlaying = false");
	if (!on) {
		Web->Focus(Windows::UI::Xaml::FocusState::Programmatic);
		PreviewEvent(L"blur", nullptr);
		return;
	}
	Preview->Focus(Windows::UI::Xaml::FocusState::Programmatic);
	PreviewEvent(L"focus", nullptr);
	// View: back to the Studio, whatever the game does with it (read here: the game has the focus).
	WeakReference weakThis(this);
	create_task([weakThis]() {
		bool armed = false;
		while (true) {
			Sleep(60);
			auto page = weakThis.Resolve<CartouchePage>();
			if (page == nullptr || !page->previewPlaying.load()) break;
			bool view = false;
			for (auto pad : Windows::Gaming::Input::Gamepad::Gamepads)
				if ((pad->GetCurrentReading().Buttons & Windows::Gaming::Input::GamepadButtons::View) == Windows::Gaming::Input::GamepadButtons::View) view = true;
			// Pressed after « Jouer » began, not still held from before.
			if (!view) armed = true;
			else if (armed) {
				page->Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([weakThis]() {
					auto inner = weakThis.Resolve<CartouchePage>();
					if (inner != nullptr) inner->PreviewPlaying(false);
				}));
				break;
			}
		}
	});
}

task<IJsonValue^> CartouchePage::PreviewCall(const std::wstring& name, JsonArray^ args) {
	WeakReference weakThis(this);
	auto none = []() { return task_from_result<IJsonValue^>(JsonValue::CreateNullValue()); };
	// [address, fingerprint]: the game the PC shares, loaded in the preview (its certificate pinned).
	if (name == L"previewOpen") {
		String^ address = args->GetStringAt(0);
		String^ origin = PreviewOriginOf(address);
		std::wstring pin(args->GetStringAt(1)->Data());
		for (auto& c : pin) c = static_cast<wchar_t>(std::towupper(c));
		if (pin.size() != 64) throw std::runtime_error("Empreinte du certificat invalide");
		previewOrigin = origin;
		previewFingerprint = ref new String(pin.c_str());
		previewLoaded = false;
		return create_task(Preview->EnsureCoreWebView2Async()).then([weakThis, address](task<void> ready) -> IJsonValue^ {
			ready.get();
			auto page = weakThis.Resolve<CartouchePage>();
			if (page == nullptr) throw std::runtime_error("Cartouche s'est fermé");
			page->WirePreview();
			page->Preview->CoreWebView2->Navigate(address);
			return JsonValue::CreateStringValue(L"xbox-preview");
		}, task_continuation_context::use_current());
	}
	auto core = Preview->CoreWebView2;
	if (name == L"previewClose") {
		PreviewPlaying(false);
		Preview->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		previewOrigin = nullptr;
		previewFingerprint = nullptr;
		previewLoaded = false;
		if (core != nullptr) core->Navigate(L"about:blank");
		return none();
	}
	// [x, y, width, height, visible, muted], in the page's pixels (the same as this page's).
	if (name == L"previewPlace") {
		double x = args->GetNumberAt(0), y = args->GetNumberAt(1), width = args->GetNumberAt(2), height = args->GetNumberAt(3);
		bool visible = args->GetBooleanAt(4) && previewOrigin != nullptr && width > 2 && height > 2;
		Preview->Margin = Thickness(x, y, 0, 0);
		Preview->Width = width > 1 ? width : 1;
		Preview->Height = height > 1 ? height : 1;
		Preview->Visibility = visible ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
		if (core != nullptr && args->Size > 5) core->IsMuted = args->GetBooleanAt(5);
		return none();
	}
	if (core == nullptr || previewOrigin == nullptr) throw std::runtime_error("L'aperçu n'est pas ouvert");
	if (name == L"previewStatus") {
		auto status = ref new JsonObject();
		status->Insert(L"loaded", JsonValue::CreateBooleanValue(previewLoaded));
		status->Insert(L"url", JsonValue::CreateStringValue(core->Source));
		status->Insert(L"perf", JsonValue::CreateBooleanValue(false));
		return task_from_result<IJsonValue^>(status);
	}
	if (name == L"previewReload") {
		core->Reload();
		return none();
	}
	// [true]: the game takes the controller (« Jouer »); [false]: the Studio has it again.
	if (name == L"previewFocus") {
		PreviewPlaying(args->Size > 0 && args->GetBooleanAt(0));
		return none();
	}
	// [id, body]: the body of an async function run in the game's page (the Studio's bridge calls,
	// shared/dock-scripts.ts); its answer comes back as a « previewResult » event with that id.
	if (name == L"previewEval") {
		String^ id = static_cast<long long>(args->GetNumberAt(0)).ToString();
		String^ body = args->GetStringAt(1);
		String^ script = L"(async () => { const id = " + id + L"; const say = (answer) => window.chrome.webview.postMessage(JSON.stringify(Object.assign({ cartouchePreview: id }, answer))); try { const value = await (async () => { " + body +
			L" })(); say({ ok: true, value: value === undefined ? null : value }); } catch (error) { say({ ok: false, error: String((error && error.message) || error) }); } })(); true";
		core->ExecuteScriptAsync(script);
		return none();
	}
	// The picture of the game, while HTML covers its place (a dialog): a JPEG data: URL.
	if (name == L"previewCapture") {
		auto stream = ref new Windows::Storage::Streams::InMemoryRandomAccessStream();
		return create_task(core->CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat::Jpeg, stream)).then([stream]() {
			stream->Seek(0);
			auto reader = ref new Windows::Storage::Streams::DataReader(stream->GetInputStreamAt(0));
			return create_task(reader->LoadAsync(static_cast<unsigned int>(stream->Size))).then([reader](unsigned int count) -> IJsonValue^ {
				auto buffer = reader->ReadBuffer(count);
				return JsonValue::CreateStringValue(L"data:image/jpeg;base64," + CryptographicBuffer::EncodeToBase64String(buffer));
			});
		});
	}
	throw std::runtime_error("Appel d'aperçu inconnu");
}
