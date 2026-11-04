//
// WebGamePage.xaml.h
// A browser game from Cartouche's catalog, full screen in the app (no PC needed). Its saves stay
// in the app's WebView2 profile, like a game opened in Cartouche for Android.
//

#pragma once

// clang-format off
#include "pch.h"
// clang-format on
#include "Pages/WebGamePage.g.h"

namespace moonlight_xbox_dx {

	[Windows::Foundation::Metadata::WebHostHidden] public ref class WebGamePage sealed {
	public:
		WebGamePage();

	protected:
		virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
		virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

	private:
		void OnBackRequested(Platform::Object^ sender, Windows::UI::Core::BackRequestedEventArgs^ args);
		void Leave();
		Windows::Foundation::EventRegistrationToken backToken;
		bool leaving = false;
	};
}
