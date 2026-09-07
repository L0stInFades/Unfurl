#include "App.h"

#include <Windows.h>
#include <MddBootstrap.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.Windows.ApplicationModel.WindowsAppRuntime.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    PACKAGE_VERSION minimum_version{};
    minimum_version.Major = 2;
    minimum_version.Minor = 4;
    minimum_version.Build = 0;
    minimum_version.Revision = 0;
    const auto bootstrap = MddBootstrapInitialize2(0x00020004, nullptr, minimum_version,
                                                   MddBootstrapInitializeOptions_OnPackageIdentity_NOOP);
    if (FAILED(bootstrap)) {
        return static_cast<int>(bootstrap);
    }
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    UINT32 package_name_length{};
    if (GetCurrentPackageFullName(&package_name_length, nullptr) == ERROR_INSUFFICIENT_BUFFER) {
        // MSIX supplies the framework; DeploymentManager enables its servicing packages.
        using namespace winrt::Microsoft::Windows::ApplicationModel::WindowsAppRuntime;
        try {
            if (DeploymentManager::GetStatus().Status() != DeploymentStatus::Ok) {
                const auto result = DeploymentManager::Initialize();
                if (result.Status() != DeploymentStatus::Ok)
                    OutputDebugStringW(L"Unfurl: Windows App Runtime servicing could not be initialized.\n");
            }
        } catch (winrt::hresult_error const&) {
            // Archive operations only require the framework installed with the app.
            OutputDebugStringW(L"Unfurl: Windows App Runtime servicing is unavailable.\n");
        }
    }
    winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) { winrt::make<winrt::Unfurl::App>(); });
    MddBootstrapShutdown();
    return 0;
}
