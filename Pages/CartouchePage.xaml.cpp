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

#include <cwctype>
#include <fstream>
#include <regex>
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

	std::vector<uint8_t> ReadAll(const std::wstring& path) {
		std::ifstream in(path, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	void WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes) {
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	IJsonValue^ DataUrl(const std::vector<uint8_t>& bytes) {
		if (bytes.empty()) return JsonValue::CreateNullValue();
		auto buffer = CryptographicBuffer::CreateFromByteArray(ArrayReference<uint8>(const_cast<uint8*>(bytes.data()), static_cast<unsigned int>(bytes.size())));
		String^ type = bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 ? L"image/jpeg" : L"image/png";
		return JsonValue::CreateStringValue(L"data:" + type + L";base64," + CryptographicBuffer::EncodeToBase64String(buffer));
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
	// While a game streams, the interface sleeps (memory and GPU go to the stream).
	Web->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
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
			if (inner == nullptr || inner->Frame == nullptr || inner->Frame->Content != inner) return;
			inner->Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([weakThis]() {
				auto again = weakThis.Resolve<CartouchePage>();
				if (again != nullptr && again->Frame != nullptr && again->Frame->Content == again) again->Web->Focus(Windows::UI::Xaml::FocusState::Programmatic);
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

// Drawn: the app is mounted (main.tsx adds has-app) and the images on screen have arrived, so
// the interface appears whole, not tile by tile; 2 s after mounting at most (a screen without
// images, lazy covers the WebView does not load while it is covered).
static const wchar_t* READY = LR"JS((() => {
  if (!document.documentElement.classList.contains("has-app")) return false;
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

	if (name == L"hello") {
		auto hello = ref new JsonObject();
		auto version = Windows::ApplicationModel::Package::Current->Id->Version;
		hello->Insert(L"version", JsonValue::CreateStringValue(version.Major + L"." + version.Minor + L"." + version.Build));
		hello->Insert(L"computers", Computers());
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
		// The PC's apps (Cartouche Stream publishes Ma collection there), each with its cover.
		return create_task([host]() -> IJsonValue^ {
			Server server(host);
			PAPP_LIST list = nullptr;
			if (gs_applist(&server.data, &list) != GS_OK) throw std::runtime_error(gs_error ? gs_error : "No games");
			LocalFolder(L"covers");
			auto folder = LocalFolder(L"covers\\" + std::wstring(Uuid(host)->Data()));
			std::string folderA = Utils::PlatformStringToStdString(ref new String(folder.c_str()));
			auto apps = ref new JsonArray();
			for (PAPP_LIST app = list; app != nullptr; app = app->next) {
				gs_appasset(&server.data, folderA.c_str(), app->id);
				auto json = ref new JsonObject();
				json->Insert(L"id", JsonValue::CreateNumberValue(app->id));
				json->Insert(L"name", JsonValue::CreateStringValue(Wide(app->name ? app->name : "")));
				json->Insert(L"hdr", JsonValue::CreateBooleanValue(false));
				json->Insert(L"cover", DataUrl(ReadAll(folder + std::to_wstring(app->id) + L".png")));
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
		// Moonlight's settings for the PC: resolution, frame rate, bitrate, codec, sound.
		MoonlightHost^ chosen = nullptr;
		for (auto host : state->SavedHosts) {
			if (host->Paired) {
				chosen = host;
				break;
			}
		}
		if (chosen == nullptr && state->SavedHosts->Size > 0) chosen = state->SavedHosts->GetAt(0);
		if (chosen == nullptr) throw std::runtime_error("Aucun PC");
		Frame->Navigate(Windows::UI::Xaml::Interop::TypeName(HostSettingsPage::typeid), chosen);
		return value(JsonValue::CreateNullValue());
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
			std::wstring file = LocalFolder(L"pc-covers") + std::wstring(Uuid(host)->Data()) + L"-" + std::wstring(id.begin(), id.end()) + L"-" + fingerprint + L".png";
			auto image = ReadAll(file);
			if (image.empty()) {
				image = ApiGet(host, "/v1/games/" + id + "/cover");
				WriteAll(file, image);
			}
			return DataUrl(image);
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
