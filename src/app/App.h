#pragma once

#include "MainPage.h"

#include <winrt/Microsoft.UI.Xaml.h>

namespace winrt::Unfurl {

struct App : Microsoft::UI::Xaml::ApplicationT<App> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

  private:
    Microsoft::UI::Xaml::Window window_{nullptr};
};

} // namespace winrt::Unfurl
