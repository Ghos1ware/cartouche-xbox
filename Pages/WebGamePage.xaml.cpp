//
// WebGamePage.xaml.cpp
// A browser game, full screen. The game's own controls take the controller (Gamepad API); B
// belongs to the game too, so leaving is View + Menu held together.
//

// clang-format off
#include "pch.h"
// clang-format on
#include "WebGamePage.xaml.h"
#include "Utils.hpp"

using namespace moonlight_xbox_dx;

using namespace concurrency;
using namespace Microsoft::Web::WebView2::Core;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Navigation;

WebGamePage::WebGamePage() {
	InitializeComponent();
}

void WebGamePage::OnNavigatedTo(NavigationEventArgs^ e) {
	Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
	backToken = SystemNavigationManager::GetForCurrentView()->BackRequested += ref new EventHandler<BackRequestedEventArgs^>(this, &WebGamePage::OnBackRequested);
	auto url = dynamic_cast<String^>(e->Parameter);
	WeakReference weakThis(this);
	create_task(Web->EnsureCoreWebView2Async()).then([weakThis, url](task<void> t) {
		try {
			t.get();
		}
		catch (...) {
			return;
		}
		auto page = weakThis.Resolve<WebGamePage>();
		if (page == nullptr || url == nullptr) return;
		auto core = page->Web->CoreWebView2;
		core->Settings->IsStatusBarEnabled = false;
		core->Settings->AreDefaultContextMenusEnabled = false;
		// A game opening another window stays in this one.
		core->NewWindowRequested += ref new TypedEventHandler<CoreWebView2^, CoreWebView2NewWindowRequestedEventArgs^>([weakThis](CoreWebView2^ sender, CoreWebView2NewWindowRequestedEventArgs^ args) {
			args->Handled = true;
			auto inner = weakThis.Resolve<WebGamePage>();
			if (inner != nullptr) inner->Web->CoreWebView2->Navigate(args->Uri);
		});
		// Edge on Xbox drives a cursor with the controller unless the page takes the controller.
		core->AddScriptToExecuteOnDocumentCreatedAsync(L"try { navigator.gamepadInputEmulation = 'gamepad'; } catch (e) {}");
		// As in the Studio's preview (CartouchePage PREVIEW_GATE): no WebGPU and no GPU timers, which brought
		// the console's GPU process down as a game made with Cartouche Studio loaded. WebGL 2 instead.
		core->AddScriptToExecuteOnDocumentCreatedAsync(LR"JS((() => {
  try { Object.defineProperty(Navigator.prototype, "gpu", { configurable: true, get: () => undefined }); } catch (e) {}
  try {
    const TIMERS = /disjoint_timer_query/i;
    for (const C of [window.WebGLRenderingContext, window.WebGL2RenderingContext].filter(Boolean)) {
      const get = C.prototype.getExtension, list = C.prototype.getSupportedExtensions;
      C.prototype.getExtension = function (name) { return TIMERS.test(String(name)) ? null : get.call(this, name); };
      C.prototype.getSupportedExtensions = function () { const all = list.call(this); return all ? all.filter((name) => !TIMERS.test(name)) : all; };
    }
  } catch (e) {}
})();)JS");
		core->Navigate(url);
		page->Web->Focus(Windows::UI::Xaml::FocusState::Programmatic);
	}, task_continuation_context::use_current());

	// View + Menu together: back to Cartouche, whatever the game does with each button.
	create_task([weakThis]() {
		while (true) {
			Sleep(100);
			auto page = weakThis.Resolve<WebGamePage>();
			if (page == nullptr || page->leaving) break;
			for (auto pad : Gamepad::Gamepads) {
				auto buttons = pad->GetCurrentReading().Buttons;
				if ((buttons & GamepadButtons::View) == GamepadButtons::View && (buttons & GamepadButtons::Menu) == GamepadButtons::Menu) {
					page->Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([weakThis]() {
						auto inner = weakThis.Resolve<WebGamePage>();
						if (inner != nullptr) inner->Leave();
					}));
					return;
				}
			}
		}
	});
}

void WebGamePage::OnNavigatedFrom(NavigationEventArgs^ e) {
	leaving = true;
	SystemNavigationManager::GetForCurrentView()->BackRequested -= backToken;
	if (Web->CoreWebView2 != nullptr) Web->Close();
}

void WebGamePage::OnBackRequested(Object^ sender, BackRequestedEventArgs^ args) {
	// B is the game's: handled, so Xbox does not suspend the app.
	args->Handled = true;
}

void WebGamePage::Leave() {
	if (leaving) return;
	leaving = true;
	if (Frame->CanGoBack) Frame->GoBack();
}
