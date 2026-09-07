#pragma once

#include "MainPage.h"

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

namespace winrt::Unfurl {

struct App : Microsoft::UI::Xaml::ApplicationT<App, Microsoft::UI::Xaml::Markup::IXamlMetadataProvider> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);
    Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& type);
    Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(hstring const& name);
    com_array<Microsoft::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions();

  private:
    Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata_{nullptr};
    Microsoft::UI::Xaml::Window window_{nullptr};
};

} // namespace winrt::Unfurl
