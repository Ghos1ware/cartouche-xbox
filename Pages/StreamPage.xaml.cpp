//
// DirectXPage.xaml.cpp
// Implementation of the DirectXPage class.
//

#include "pch.h"
#include "StreamPage.xaml.h"
#include <cwctype>
#include "../Plot/ImGuiPlots.h"
#include "../Streaming/AudioPlayer.h"
#include "../Streaming/FrameQueue.h"
#include "../Streaming/FFMpegDecoder.h"
#include <Utils.hpp>
#include <KeyboardControl.xaml.h>
#include "../Common/ModalDialog.xaml.h"
#include "../Utils/TracyBootstrap.h"

using namespace moonlight_xbox_dx;

using namespace Platform;
using namespace Platform::Collections;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Gaming::Input;
using namespace Windows::Graphics::Display;
using namespace Windows::Graphics::Display::Core;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::UI::ViewManagement;
using namespace Windows::UI::ViewManagement::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace concurrency;

StreamPage::StreamPage():
	m_windowVisible(true),
	m_coreInput(nullptr)
{
	InitializeComponent();

	// Init Tracy, all Tracy functions are noops if not enabled.
	TracyBootstrap::Initialize();

	DisplayInformation^ currentDisplayInformation = DisplayInformation::GetForCurrentView();
	NavigationCacheMode = Windows::UI::Xaml::Navigation::NavigationCacheMode::Enabled;
	swapChainPanel->SizeChanged +=
		ref new SizeChangedEventHandler(this, &StreamPage::OnSwapChainPanelSizeChanged);
	m_deviceResources = std::make_shared<DX::DeviceResources>();
}

StreamPage::~StreamPage()
{
}

void StreamPage::OnBackRequested(Platform::Object^ e,Windows::UI::Core::BackRequestedEventArgs^ args)
{
	// UWP on Xbox One triggers a back request whenever the B
	// button is pressed which can result in the app being
	// suspended if unhandled
	args->Handled = true;
	// B closes Cartouche's menu.
	if (menuOpen) CloseMenu();
}

void StreamPage::Page_Loaded(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ e) {

	this->m_progressView->Visibility = Windows::UI::Xaml::Visibility::Visible;
	PrepareLaunch();

	if (Utils::ShowDevTools()) {
		toggleCapture->Visibility = Windows::UI::Xaml::Visibility::Visible;
	}

	auto navigation = Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
	m_back_cookie = navigation->BackRequested += ref new EventHandler<BackRequestedEventArgs ^>(this, &StreamPage::OnBackRequested);

	Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);

	keyDownHandler = (Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyDown += ref new Windows::Foundation::TypedEventHandler<Windows::UI::Core::CoreWindow ^, Windows::UI::Core::KeyEventArgs ^>(this, &StreamPage::OnKeyDown));
	keyUpHandler = (Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyUp += ref new Windows::Foundation::TypedEventHandler<Windows::UI::Core::CoreWindow ^, Windows::UI::Core::KeyEventArgs ^>(this, &StreamPage::OnKeyUp));

	// Detect gamepad connection and disconnection events
	gamepadAddedHandler = Gamepad::GamepadAdded += ref new EventHandler<Gamepad^>(this, &StreamPage::OnGamepadAdded);
	gamepadRemovedHandler = Gamepad::GamepadRemoved += ref new EventHandler<Gamepad ^>(this, &StreamPage::OnGamepadRemoved);

	if (IsXbox() && m_hdmiInfo == nullptr) {
		auto hdmi = HdmiDisplayInformation::GetForCurrentView();
		if (hdmi != nullptr) {
			// Watch for HDMI display changes
			m_hdmiHandler = hdmi->DisplayModesChanged +=
				ref new TypedEventHandler<HdmiDisplayInformation^, Platform::Object^>(this, &StreamPage::OnHdmiDisplayModesChanged);

			m_hdmiInfo = hdmi;
			Utils::Log("HDMI display listener attached\n");
		}
	}

	try {
		m_deviceResources->SetSwapChainPanel(swapChainPanel);
	} catch (...) {
		Utils::Log("StreamPage::Page_Loaded: SetSwapChainPanel failed\n");
	}

	Platform::WeakReference weakThis(this);
	DISPATCH_UI([weakThis] {
		auto that = weakThis.Resolve<StreamPage>();
		if (that == nullptr) return;
		try {
			that->m_main = std::unique_ptr<moonlight_xbox_dxMain>(new moonlight_xbox_dxMain(that->m_deviceResources, that, new MoonlightClient(), that->configuration));
			that->m_main->CreateDeviceDependentResources();
			that->m_main->CreateWindowSizeDependentResources();
			that->m_main->StartRenderLoop();
        } catch (const std::exception &ex) {
			Utils::Logf("StreamPage::Page_Loaded: Exception when starting stream. Exception: %s", ex.what());
        } catch (const std::string &string) {
			Utils::Logf("StreamPage::Page_Loaded: Exception when starting stream. Exception: %s", string);
        } catch (Platform::Exception ^ e) {
            Platform::String ^ errorMsg = ref new Platform::String();
            errorMsg = errorMsg->Concat(L"Exception: ", e->Message);
            errorMsg = errorMsg->Concat(errorMsg, Utils::StringPrintf("%x", e->HResult));
			Utils::Logf("StreamPage::Page_Loaded: Exception when starting stream. Exception: %s", Utils::PlatformStringToStdString(errorMsg));
        } catch (...) {
            Utils::Log("StreamPage::Page_Loaded: Exception when starting stream. Exception: Generic Exception");
        }
	});
}

void StreamPage::Page_Unloaded(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ e) {
	auto navigation = Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
	navigation->BackRequested -= m_back_cookie;

	Gamepad::GamepadAdded -= gamepadAddedHandler;
	Gamepad::GamepadRemoved -= gamepadRemovedHandler;

	if (m_hdmiInfo != nullptr) {
		m_hdmiInfo->DisplayModesChanged -= m_hdmiHandler;
		m_hdmiInfo = nullptr;
	}

	if (this->m_main) {

		Utils::Log("StreamPage::Page_Unloaded stopping m_main render loop\n");

		try {
			this->m_main->StopRenderLoop();
			this->m_main.reset();
		} catch (std::exception &ex) {
			Utils::Logf("StreamPage::Page_Unloaded m_main threw an exception: %s\n", ex.what());
		} catch (...) {
			Utils::Log("StreamPage::Page_Unloaded m_main threw an exception\n");
		}

		Utils::Log("StreamPage::Page_Unloaded m_main reset\n");
	}

	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyDown -= keyDownHandler;
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyUp -= keyUpHandler;
}

void StreamPage::OnSwapChainPanelSizeChanged(Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e)
{
	if (m_main == nullptr || m_deviceResources == nullptr)return;
	Utils::Logf("StreamPage::OnSwapChainPanelSizeChanged( NewSize: %f x %f )\n", e->NewSize.Width, e->NewSize.Height);
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	m_deviceResources->SetLogicalSize(e->NewSize);
	m_main->CreateDeviceDependentResources();
	m_main->CreateWindowSizeDependentResources();
}


void StreamPage::flyoutButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	// Capture stops itself when it hits the size limit, so resync the menu text.
	this->CaptureMode = FFMpegDecoder::instance().IsCaptureActive();

	Windows::UI::Xaml::Controls::Flyout::ShowAttachedFlyout((FrameworkElement^)sender);
	m_main->SetFlyoutOpened(true);
}


void StreamPage::ActionsFlyout_Closed(Platform::Object^ sender, Platform::Object^ e)
{
	if (m_main != nullptr) m_main->SetFlyoutOpened(false);
}

void StreamPage::toggleMouseButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	SetMouseMode(!this->m_mouseMode);
}

void StreamPage::SetMouseMode(bool enabled)
{
	this->MouseMode = enabled;
	if (m_main) m_main->mouseMode = this->MouseMode;
}

void StreamPage::showKeyboardButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	if (!m_main) return;
	if (GetApplicationState()->EnableKeyboard) {
		m_main->keyboardMode = true;

		this->Dispatcher->RunAsync(
			Windows::UI::Core::CoreDispatcherPriority::Normal,
			ref new Windows::UI::Core::DispatchedHandler([this]() {
				this->m_keyboardView->Visibility = Windows::UI::Xaml::Visibility::Visible;
			}));
	} else {
		CoreInputView::GetForCurrentView()->TryShow(CoreInputViewKind::Keyboard);
	}
}

void StreamPage::toggleLogsButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	bool isVisible = m_main->ToggleLogs();
	SetShowLogs(isVisible);
}

void StreamPage::SetShowLogs(bool enabled) {
	this->ShowLogs = enabled;
}

void StreamPage::toggleStatsButton_Click(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ e) {
	bool isVisible = m_main->ToggleStats();
	SetShowStats(isVisible);
}

void StreamPage::SetShowStats(bool enabled) {
	this->ShowStats = enabled;
}

void StreamPage::OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) {

	configuration = dynamic_cast<StreamConfiguration^>(e->Parameter);
	SetStreamConfig(configuration);

	if (configuration == nullptr)return;

	SetCaptureMode(false);
	SetMouseMode(false);
	SetShowLogs(false);
	SetShowStats(configuration->enableStats);

}

void StreamPage::disonnectButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyDown -= keyDownHandler;
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyUp -= keyUpHandler;

	// trigger the server disconnected flow
	this->m_main->moonlightClient->SetConnectionTerminated();
}

void StreamPage::OnKeyDown(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::KeyEventArgs^ e)
{
	//Ignore Gamepad input
	if (e->VirtualKey >= Windows::System::VirtualKey::GamepadA && e->VirtualKey <= Windows::System::VirtualKey::GamepadRightThumbstickLeft) {
		return;
	}
	auto window = CoreWindow::GetForCurrentThread();
	char modifiers = 0;
	modifiers |= window->GetKeyState(Windows::System::VirtualKey::Control) == (CoreVirtualKeyStates::Down) ? MODIFIER_CTRL : 0;
	modifiers |= window->GetKeyState(Windows::System::VirtualKey::Menu) == (CoreVirtualKeyStates::Down) ? MODIFIER_ALT : 0;
	modifiers |= window->GetKeyState(Windows::System::VirtualKey::Shift) == (CoreVirtualKeyStates::Down) ? MODIFIER_SHIFT : 0;
	if (window->GetKeyState(Windows::System::VirtualKey::LeftWindows) == CoreVirtualKeyStates::Down ||
		window->GetKeyState(Windows::System::VirtualKey::RightWindows) == CoreVirtualKeyStates::Down) {
		Utils::Log("Windows key down\n");
		modifiers |= MODIFIER_META;
	}
	this->m_main->OnKeyDown((unsigned short)e->VirtualKey, modifiers);
}


void StreamPage::OnKeyUp(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::KeyEventArgs^ e)
{
	//Ignore Gamepad input
	if (e->VirtualKey >= Windows::System::VirtualKey::GamepadA && e->VirtualKey <= Windows::System::VirtualKey::GamepadRightThumbstickLeft) {
		return;
	}
	auto window = CoreWindow::GetForCurrentThread();
	char modifiers = 0;
	modifiers |= window->GetKeyState(Windows::System::VirtualKey::Control) == (CoreVirtualKeyStates::Down) ? MODIFIER_CTRL : 0;
	modifiers |= window->GetKeyState(Windows::System::VirtualKey::Menu) == (CoreVirtualKeyStates::Down) ? MODIFIER_ALT : 0;
	modifiers |= window->GetKeyState(Windows::System::VirtualKey::Shift) == (CoreVirtualKeyStates::Down) ? MODIFIER_SHIFT : 0;
	if (window->GetKeyState(Windows::System::VirtualKey::LeftWindows) == CoreVirtualKeyStates::Down ||
		window->GetKeyState(Windows::System::VirtualKey::RightWindows) == CoreVirtualKeyStates::Down) {
		Utils::Log("Windows key up\n");
		modifiers |= MODIFIER_META;
	}
	this->m_main->OnKeyUp((unsigned short)e->VirtualKey, modifiers);
}

void StreamPage::disconnectAndCloseButton_Click(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ e) {
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyDown -= keyDownHandler;
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyUp -= keyUpHandler;
	if (this->m_main) {
		// trigger the server disconnected flow which will cleanly exit the loop and call StopRenderLoop()
		this->m_main->moonlightClient->SetConnectionTerminated();
	}

	auto that = this;

	// Cartouche's screen says it, while the PC closes the game.
	ShowClosing();
	int progressToken = 0;

	concurrency::create_task(concurrency::create_async([that, progressToken]() {
		try {
			if (that->m_main) {
				that->m_main->CloseApp();
			}
		} catch (...) {
		}
	})).then([that, progressToken](concurrency::task<void> t) {
		try {
			t.get();

			// UI is sent back to HostSelectorPage in StartRenderLoop(), after the loop exits
			// All we need to do is close the progress dialog

			(void)progressToken;
		} catch (...) {
		}
	});
}

void StreamPage::Keyboard_OnKeyDown(KeyboardControl^ sender, KeyEvent^ e)
{
	this->m_main->OnKeyDown(e->VirtualKey, e->Modifiers);
}


void StreamPage::Keyboard_OnKeyUp(KeyboardControl^ sender, KeyEvent^ e)
{
	this->m_main->OnKeyUp(e->VirtualKey, e->Modifiers);
}


void StreamPage::guideButtonShort_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	this->m_main->SendGuideButton(500);
}


void StreamPage::guideButtonLong_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	this->m_main->SendGuideButton(3000);
}


void StreamPage::toggleHDR_WinAltB_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	this->m_main->SendWinAltB();
}

void StreamPage::resetDecoder_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	// start with a fresh IDR
	LiRequestIdrFrame();

	// This also is a handy place to flush the queue
	FrameQueue::instance().clear();
	ImGuiPlots::instance().clearData(PLOT_QUEUED_FRAMES);
}

void StreamPage::toggleFramePacing_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	// thread safe atomic bool
	bool isImmediate = Pacer::instance().getPacingImmediate();
	Pacer::instance().setPacingImmediate(isImmediate ? false : true);
}

void StreamPage::toggleCapture_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	SetCaptureMode(!this->CaptureMode);
}

void StreamPage::SetCaptureMode(bool wanted)
{
	this->CaptureMode = wanted;
	FFMpegDecoder::instance().SetCapture(wanted);
}

// Audio buffer slider

void StreamPage::audioBufferSlider_Loaded(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^) {
	m_audioBufferSlider = safe_cast<Windows::UI::Xaml::Controls::Slider ^>(sender);

	// Seed from the live value; ValueChanged fires during template load with
	// the coerced default, which must not be pushed back into AudioPlayer.
	m_audioBufferSliderReady = false;
	m_audioBufferSlider->Value = AudioPlayer::instance().GetAudioBufferMs();
	m_audioBufferSliderReady = true;
}

void StreamPage::audioBufferSlider_ValueChanged(Platform::Object ^ sender, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs ^ args) {
	if (!m_audioBufferSliderReady) {
		return;
	}

	int audioBufferMs = static_cast<int>(args->NewValue);
	AudioPlayer::instance().SetAudioBufferMs(audioBufferMs);
}

void StreamPage::audioBufferSliderItem_PreviewKeyDown(Platform::Object ^, Windows::UI::Xaml::Input::KeyRoutedEventArgs ^ args) {
	if (m_audioBufferSlider == nullptr) {
		return;
	}

	double change = 0;

	switch (args->Key) {
	case Windows::System::VirtualKey::Left:
	case Windows::System::VirtualKey::GamepadDPadLeft:
	case Windows::System::VirtualKey::GamepadLeftThumbstickLeft:
		change = -10;
		break;

	case Windows::System::VirtualKey::Right:
	case Windows::System::VirtualKey::GamepadDPadRight:
	case Windows::System::VirtualKey::GamepadLeftThumbstickRight:
		change = 10;
		break;

	default:
		return;
	}

	double value = m_audioBufferSlider->Value + change;

	if (value < m_audioBufferSlider->Minimum) {
		value = m_audioBufferSlider->Minimum;
	} else if (value > m_audioBufferSlider->Maximum) {
		value = m_audioBufferSlider->Maximum;
	}

	m_audioBufferSlider->Value = value;
	args->Handled = true;
}

// Glitch counter

void StreamPage::audioGlitchText_Loaded(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^) {
	m_audioGlitchText = safe_cast<Windows::UI::Xaml::Controls::TextBlock ^>(sender);
	UpdateAudioGlitchText();
}

void StreamPage::UpdateAudioGlitchText() {
	if (m_audioGlitchText == nullptr) {
		return;
	}


	m_audioGlitchText->Text = Utils::StringFromStdString(
		"Gr\xC3\xA9sillements : " + std::to_string(AudioPlayer::instance().GetGlitchCount()) +
		" \xC2\xB7 Rattrapages de latence : " + std::to_string(AudioPlayer::instance().GetLatencyDropCount()));
}

// Event handlers

void StreamPage::OnPropertyChanged(Platform::String^ propertyName)
{
	PropertyChanged(this, ref new Windows::UI::Xaml::Data::PropertyChangedEventArgs(propertyName));
}

void StreamPage::OnGamepadAdded(Platform::Object^ sender, Gamepad^ gamepad)
{
	m_refreshGamepads.store(true, std::memory_order_release);
}

void StreamPage::OnGamepadRemoved(Platform::Object^ sender, Gamepad^ gamepad)
{
	m_refreshGamepads.store(true, std::memory_order_release);
}

bool StreamPage::ShouldRefreshGamepads() {
	return m_refreshGamepads.exchange(false);
}

void StreamPage::RequestRefreshGamepads() {
	m_refreshGamepads.store(true, std::memory_order_release);
}

void StreamPage::OnHdmiDisplayModesChanged(HdmiDisplayInformation^, Platform::Object^) {
	Utils::Log("HDMI display mode changed, requesting refresh\n");
	m_refreshDisplay.store(true, std::memory_order_release);
}

bool StreamPage::ShouldRefreshDisplay() {
	return m_refreshDisplay.exchange(false);
}

// ---- Cartouche's launch screen ----

void StreamPage::PrepareLaunch() {
	launchFailed = false;
	auto name = configuration != nullptr && configuration->appName != nullptr ? configuration->appName : L"Cartouche";
	GameTitle->Text = name;
	auto pc = configuration != nullptr && configuration->computerName != nullptr && !configuration->computerName->IsEmpty() ? configuration->computerName : L"ton PC";
	std::wstring kicker = L"LANCEMENT SUR " + std::wstring(pc->Data());
	std::transform(kicker.begin(), kicker.end(), kicker.begin(), ::towupper);
	Kicker->Text = ref new Platform::String(kicker.c_str());
	Kicker->Foreground = ref new Windows::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0xF2, 0xC1, 0x4E));
	StatusText->Text = pc + L" lance " + name + L"\u2026";
	StepText->Text = L"";
	FailActions->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	FailLog->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	LaunchHint->Visibility = Windows::UI::Xaml::Visibility::Visible;
	// The cover the app keeps for the game (CartouchePage); without one, Cartouche's cartridge.
	auto cover = configuration != nullptr && configuration->coverUri != nullptr && !configuration->coverUri->IsEmpty()
		? configuration->coverUri : L"ms-appx:///Assets/Square150x150Logo.scale-400.png";
	auto image = ref new Windows::UI::Xaml::Media::Imaging::BitmapImage(ref new Windows::Foundation::Uri(cover));
	Cover->Source = image;
	Backdrop->Source = image;
	ProgressView->Opacity = 1;
	BarScale->ScaleX = 0.04;
	MoveBar(0.1);
	LaunchIn->Begin();
	LaunchRun->Begin();
}

void StreamPage::MoveBar(double to) {
	auto story = ref new Windows::UI::Xaml::Media::Animation::Storyboard();
	auto grow = ref new Windows::UI::Xaml::Media::Animation::DoubleAnimation();
	grow->To = to;
	Windows::Foundation::TimeSpan span;
	span.Duration = 6000000;
	grow->Duration = Windows::UI::Xaml::Duration(span);
	auto ease = ref new Windows::UI::Xaml::Media::Animation::CubicEase();
	ease->EasingMode = Windows::UI::Xaml::Media::Animation::EasingMode::EaseOut;
	grow->EasingFunction = ease;
	Windows::UI::Xaml::Media::Animation::Storyboard::SetTarget(grow, BarScale);
	Windows::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(grow, L"ScaleX");
	story->Children->Append(grow);
	story->Begin();
}

void StreamPage::SetLaunchStage(int stage) {
	if (launchFailed) return;
	// Moonlight's stages (Limelight.h), in the player's words.
	const wchar_t* line = L"Connexion au PC\u2026";
	if (stage >= STAGE_VIDEO_STREAM_START) line = L"R\u00e9ception de l'image et du son\u2026";
	else if (stage >= STAGE_CONTROL_STREAM_INIT) line = L"Ouverture du flux\u2026";
	else if (stage >= STAGE_AUDIO_STREAM_INIT) line = L"Le PC pr\u00e9pare le jeu\u2026";
	if (stage > 0) StatusText->Text = ref new Platform::String(line);
	MoveBar(0.12 + 0.8 * std::min(stage, STAGE_MAX) / (double)STAGE_MAX);
}

void StreamPage::HideLaunch() {
	if (launchFailed) return;
	StatusText->Text = L"C'est parti !";
	MoveBar(1.0);
	auto that = this;
	LaunchOut->Completed += ref new Windows::Foundation::EventHandler<Platform::Object^>([that](Platform::Object^, Platform::Object^) {
		that->ProgressView->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		that->LaunchRun->Stop();
	});
	LaunchOut->Begin();
}

void StreamPage::ShowLaunchFailure(Platform::String^ message) {
	launchFailed = true;
	LaunchOut->Stop();
	ProgressView->Opacity = 1;
	ProgressView->Visibility = Windows::UI::Xaml::Visibility::Visible;
	LaunchRun->Stop();
	Kicker->Text = L"LE STREAM N'A PAS D\u00c9MARR\u00c9";
	Kicker->Foreground = ref new Windows::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0xFF, 0x8A, 0x9B));
	StatusText->Text = L"Le PC n'a pas pu lancer le jeu, ou la connexion s'est coup\u00e9e. V\u00e9rifie que Cartouche est ouvert sur le PC.";
	StepText->Text = message;
	BarFrom->Color = Windows::UI::ColorHelper::FromArgb(255, 0xFF, 0x6B, 0x81);
	BarTo->Color = Windows::UI::ColorHelper::FromArgb(255, 0xFF, 0x8A, 0x9B);
	LaunchHint->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	FailActions->Visibility = Windows::UI::Xaml::Visibility::Visible;
	FailBack->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void StreamPage::ShowClosing() {
	LaunchOut->Stop();
	ProgressView->Opacity = 1;
	ProgressView->Visibility = Windows::UI::Xaml::Visibility::Visible;
	Kicker->Text = L"FERMETURE";
	auto name = configuration != nullptr && configuration->appName != nullptr ? configuration->appName : L"le jeu";
	StatusText->Text = L"On ferme " + name + L" sur le PC\u2026";
	StepText->Text = L"";
	LaunchHint->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	BarScale->ScaleX = 0.3;
	LaunchRun->Begin();
}

void StreamPage::FailBack_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	if (m_main) {
		m_main->StopRenderLoop();
		m_main->ExitStreamPage();
	}
}

void StreamPage::FailLogs_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	if (FailLog->Visibility == Windows::UI::Xaml::Visibility::Visible) {
		FailLog->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		return;
	}
	std::wstring text;
	auto lines = Utils::GetLogLines();
	for (size_t i = lines.size() > 12 ? lines.size() - 12 : 0; i < lines.size(); i++) text += lines[i];
	FailLog->Text = ref new Platform::String(text.c_str());
	FailLog->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

// ---- Cartouche's in-game menu ----

void StreamPage::OpenMenu() {
	if (menuOpen) return;
	menuOpen = true;
	if (m_main) m_main->SetFlyoutOpened(true);
	auto name = configuration != nullptr && configuration->appName != nullptr ? configuration->appName : L"Cartouche";
	MenuTitle->Text = name;
	auto pc = configuration != nullptr && configuration->computerName != nullptr && !configuration->computerName->IsEmpty() ? configuration->computerName : L"ton PC";
	std::wstring kicker = L"EN JEU SUR " + std::wstring(pc->Data());
	std::transform(kicker.begin(), kicker.end(), kicker.begin(), ::towupper);
	MenuKicker->Text = ref new Platform::String(kicker.c_str());
	if (configuration != nullptr) {
		std::wstring detail = std::to_wstring(configuration->width) + L"\u00d7" + std::to_wstring(configuration->height) + L" \u00b7 " +
			std::to_wstring(configuration->FPS) + L" i/s \u00b7 " + std::to_wstring(configuration->bitrate / 1000) + L" Mb/s";
		if (configuration->videoCodec != nullptr) detail += L" \u00b7 " + std::wstring(configuration->videoCodec == L"H.264" ? L"H.264" : L"HEVC");
		MenuDetail->Text = ref new Platform::String(detail.c_str());
	}
	auto cover = configuration != nullptr && configuration->coverUri != nullptr && !configuration->coverUri->IsEmpty()
		? configuration->coverUri : L"ms-appx:///Assets/Square150x150Logo.scale-400.png";
	MenuCover->Source = ref new Windows::UI::Xaml::Media::Imaging::BitmapImage(ref new Windows::Foundation::Uri(cover));
	// The stream fills the whole TV; a TV may crop its edges. The panel's content stays in the
	// console's safe area (VisibleBounds), with room for a selected tile to grow.
	auto window = Windows::UI::Xaml::Window::Current->Bounds;
	auto safe = Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->VisibleBounds;
	// When the console reports the whole screen as safe, Microsoft's TV margins (48 and 27).
	auto inset = [](double v, double tv) { return v > 0.5 ? v : (IsXbox() ? tv : 0.0); };
	MenuContent->Margin = Windows::UI::Xaml::Thickness(40, 36 + inset(safe.Y - window.Y, 27), 40 + inset((window.X + window.Width) - (safe.X + safe.Width), 48), 28 + inset((window.Y + window.Height) - (safe.Y + safe.Height), 27));
	RefreshMenu();
	MenuOut->Stop();
	Menu->Visibility = Windows::UI::Xaml::Visibility::Visible;
	MenuIn->Begin();
	MenuResume->Focus(Windows::UI::Xaml::FocusState::Keyboard);
}

void StreamPage::CloseMenu() {
	if (!menuOpen) return;
	menuOpen = false;
	auto that = this;
	MenuOut->Completed += ref new Windows::Foundation::EventHandler<Platform::Object^>([that](Platform::Object^, Platform::Object^) {
		if (!that->menuOpen) that->Menu->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	});
	MenuOut->Begin();
	// The controller goes back to the game.
	if (m_main) m_main->SetFlyoutOpened(false);
}

void StreamPage::RefreshMenu() {
	auto state = [](bool on) { return on ? L"Activ\u00e9" : L"D\u00e9sactiv\u00e9"; };
	MenuMouseState->Text = ref new Platform::String(state(MouseMode));
	MenuStatsState->Text = ref new Platform::String(state(ShowStats));
	MenuLogsState->Text = ref new Platform::String(state(ShowLogs));
	MenuPacingState->Text = Pacer::instance().getPacingImmediate() ? L"R\u00e9activit\u00e9" : L"Fluidit\u00e9";
}

void StreamPage::MenuResume_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) { CloseMenu(); }

void StreamPage::MenuBack_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	CloseMenu();
	disonnectButton_Click(sender, e);
}

void StreamPage::MenuQuit_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	CloseMenu();
	disconnectAndCloseButton_Click(sender, e);
}

void StreamPage::MenuMouse_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	toggleMouseButton_Click(sender, e);
	CloseMenu();
}

void StreamPage::MenuKeyboard_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	CloseMenu();
	showKeyboardButton_Click(sender, e);
}

void StreamPage::MenuStats_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	toggleStatsButton_Click(sender, e);
	RefreshMenu();
}

void StreamPage::MenuGuide_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	CloseMenu();
	guideButtonShort_Click(sender, e);
}

void StreamPage::MenuPacing_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	toggleFramePacing_Click(sender, e);
	RefreshMenu();
}

void StreamPage::MenuHdr_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	CloseMenu();
	toggleHDR_WinAltB_Click(sender, e);
}

void StreamPage::MenuDecoder_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	resetDecoder_Click(sender, e);
	CloseMenu();
}

void StreamPage::MenuLogs_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e) {
	toggleLogsButton_Click(sender, e);
	RefreshMenu();
}
