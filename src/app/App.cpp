#include "App.h"

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::Unfurl {

App::App() {
}

void App::OnLaunched(LaunchActivatedEventArgs const& args) {
    window_ = Window();
    window_.Title(L"Unfurl");
    window_.ExtendsContentIntoTitleBar(true);
    window_.SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop());
    auto page = MainPage();
    window_.Content(page);
    window_.Activate();

    const auto argument = args.Arguments();
    if (!argument.empty()) {
        page.LoadPath(argument);
    }
}

} // namespace winrt::Unfurl
