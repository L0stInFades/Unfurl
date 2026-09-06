#include "App.h"

#include <Windows.h>
#include <MddBootstrap.h>
#include <winrt/Microsoft.UI.Xaml.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    PACKAGE_VERSION minimum_version{};
    minimum_version.Major = 2;
    minimum_version.Minor = 4;
    minimum_version.Build = 0;
    minimum_version.Revision = 0;
    const auto bootstrap =
        MddBootstrapInitialize2(0x00020004, nullptr, minimum_version, MddBootstrapInitializeOptions_None);
    if (FAILED(bootstrap)) {
        return static_cast<int>(bootstrap);
    }
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) { winrt::make<winrt::Unfurl::App>(); });
    MddBootstrapShutdown();
    return 0;
}
