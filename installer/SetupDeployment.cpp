#include "SetupDeployment.h"
#include "SetupPayload.h"

#include <MddBootstrap.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>

#include <array>
#include <fstream>
#include <span>
#include <vector>

namespace unfurl::setup {
namespace {

struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() {
        if (value)
            WinHttpCloseHandle(value);
    }
};
struct Store {
    HCERTSTORE value{};
    ~Store() {
        if (value)
            CertCloseStore(value, 0);
    }
};
struct Certificate {
    PCCERT_CONTEXT value{};
    ~Certificate() {
        if (value)
            CertFreeCertificateContext(value);
    }
};

void check_cancelled(Session const& session) {
    if (session.cancelled.load(std::memory_order_relaxed))
        throw winrt::hresult_canceled();
}

std::span<const std::byte> resource(int id) {
    const auto module = GetModuleHandleW(nullptr);
    const auto info = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!info)
        winrt::throw_last_error();
    const auto data = LockResource(LoadResource(module, info));
    if (!data)
        winrt::throw_last_error();
    return {static_cast<const std::byte*>(data), SizeofResource(module, info)};
}

void save_resource(int id, std::filesystem::path const& path) {
    const auto bytes = resource(id);
    winrt::file_handle file(
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        winrt::throw_last_error();
    DWORD written{};
    if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
        winrt::throw_last_error();
    if (written != bytes.size())
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
}

Certificate embedded_certificate() {
    const auto bytes = resource(10);
    auto certificate = CertCreateCertificateContext(X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(bytes.data()),
                                                    static_cast<DWORD>(bytes.size()));
    if (!certificate)
        winrt::throw_last_error();
    if (CertVerifyTimeValidity(nullptr, certificate->pCertInfo) != 0) {
        CertFreeCertificateContext(certificate);
        throw winrt::hresult_error(CERT_E_EXPIRED, L"安装证书已过期，请下载最新安装程序。");
    }
    return Certificate{certificate};
}

bool signature_valid(std::filesystem::path const& path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status = WinVerifyTrust(nullptr, &policy, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);
    return status == ERROR_SUCCESS;
}

void download(Session const& session, wchar_t const* address, wchar_t const* expected,
              std::filesystem::path const& destination) {
    check_cancelled(session);
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(address, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        throw winrt::hresult_invalid_argument(L"安装组件地址无效。");
    InternetHandle internet{WinHttpOpen(L"UnfurlSetup/0.1.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!internet.value)
        winrt::throw_last_error();
    WinHttpSetTimeouts(internet.value, 15000, 15000, 30000, 30000);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    InternetHandle connection{WinHttpConnect(internet.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value)
        winrt::throw_last_error();
    const auto path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                      std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    InternetHandle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value)
        winrt::throw_last_error();
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr))
        winrt::throw_last_error();
    DWORD status{}, status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status,
                             &status_size, nullptr))
        winrt::throw_last_error();
    if (status != 200)
        throw winrt::hresult_error(E_FAIL, L"无法下载安装组件（HTTP " + std::to_wstring(status) + L"）。");

    struct Hash {
        BCRYPT_HASH_HANDLE value{};
        ~Hash() {
            if (value)
                BCryptDestroyHash(value);
        }
    } hash;
    winrt::check_nt(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash.value, nullptr, 0, nullptr, 0, 0));
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        throw winrt::hresult_error(E_ACCESSDENIED, L"无法保存安装组件。");
    std::array<unsigned char, 64 * 1024> buffer;
    std::uint64_t total{};
    for (;;) {
        check_cancelled(session);
        DWORD count{};
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count))
            winrt::throw_last_error();
        if (!count)
            break;
        total += count;
        if (total > 256ULL * 1024 * 1024)
            throw winrt::hresult_error(E_FAIL, L"安装组件大小异常。");
        output.write(reinterpret_cast<const char*>(buffer.data()), count);
        if (!output)
            throw winrt::hresult_error(E_FAIL, L"无法写入安装组件，请检查剩余磁盘空间。");
        winrt::check_nt(BCryptHashData(hash.value, buffer.data(), count, 0));
    }
    output.close();
    if (!output)
        throw winrt::hresult_error(E_FAIL, L"无法完成组件下载。");
    std::array<unsigned char, 32> digest{};
    winrt::check_nt(BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0));
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring actual;
    for (const auto value : digest) {
        actual.push_back(hex[value >> 4]);
        actual.push_back(hex[value & 15]);
    }
    if (actual != expected || !signature_valid(destination))
        throw winrt::hresult_error(TRUST_E_BAD_DIGEST, L"组件完整性或微软签名校验失败，请重新下载。");
}

std::filesystem::path executable_path() {
    std::wstring value(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!size || size >= value.size())
        winrt::throw_last_error();
    value.resize(size);
    return value;
}

} // namespace

Session::Session() {
    winrt::handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()))
        winrt::throw_last_error();
    DWORD size{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> user(size);
    if (!GetTokenInformation(token.get(), TokenUser, user.data(), size, &size))
        winrt::throw_last_error();
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid))
        winrt::throw_last_error();
    const std::wstring sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        winrt::throw_last_error();
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    wchar_t temporary[MAX_PATH + 1]{};
    const auto length = GetTempPathW(MAX_PATH, temporary);
    if (!length || length >= MAX_PATH) {
        LocalFree(descriptor);
        winrt::throw_last_error();
    }
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    wchar_t identifier[40]{};
    StringFromGUID2(id, identifier, 40);
    directory = std::filesystem::path(temporary) / (L"UnfurlSetup-" + std::wstring(identifier));
    const auto created = CreateDirectoryW(directory.c_str(), &security);
    const auto error = GetLastError();
    LocalFree(descriptor);
    if (!created)
        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
}

Session::~Session() {
    std::error_code ignored;
    if (!keep_feed) {
        std::filesystem::remove_all(directory, ignored);
    } else {
        // App Installer reads the feed asynchronously; retain only that small file.
        for (std::filesystem::directory_iterator it(directory, ignored), end; !ignored && it != end;
             it.increment(ignored))
            if (it->path().extension() != L".appinstaller")
                std::filesystem::remove(it->path(), ignored);
    }
}

Bootstrap::Bootstrap(Session const& session) {
    const auto path = session.directory / L"Microsoft.WindowsAppRuntime.Bootstrap.dll";
    save_resource(11, path);
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_)
        winrt::throw_last_error();
}

HRESULT Bootstrap::Initialize() {
    const auto initialize =
        reinterpret_cast<decltype(&MddBootstrapInitialize2)>(GetProcAddress(module_, "MddBootstrapInitialize2"));
    if (!initialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    PACKAGE_VERSION minimum{};
    minimum.Major = 2;
    minimum.Minor = 4;
    const auto result = initialize(0x00020004, nullptr, minimum, MddBootstrapInitializeOptions_None);
    initialized_ = SUCCEEDED(result);
    return result;
}

Bootstrap::~Bootstrap() {
    if (initialized_) {
        const auto shutdown =
            reinterpret_cast<decltype(&MddBootstrapShutdown)>(GetProcAddress(module_, "MddBootstrapShutdown"));
        if (shutdown)
            shutdown();
    }
    if (module_)
        FreeLibrary(module_);
}

std::wstring error_text(winrt::hresult_error const& error) {
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(error.code().value));
    return std::wstring(error.message()) + L"\n" + code;
}

bool certificate_trusted() {
    const auto certificate = embedded_certificate();
    Store store{CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG, L"TrustedPeople")};
    if (!store.value)
        return false;
    Certificate found{
        CertFindCertificateInStore(store.value, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, certificate.value, nullptr)};
    return found.value && found.value->cbCertEncoded == certificate.value->cbCertEncoded &&
           memcmp(found.value->pbCertEncoded, certificate.value->pbCertEncoded, certificate.value->cbCertEncoded) == 0;
}

int trust_certificate() {
    const auto certificate = embedded_certificate();
    Store store{CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                              CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, L"TrustedPeople")};
    if (!store.value)
        winrt::throw_last_error();
    if (!CertAddCertificateContextToStore(store.value, certificate.value, CERT_STORE_ADD_USE_EXISTING, nullptr))
        winrt::throw_last_error();
    return ERROR_SUCCESS;
}

void request_certificate_trust(Session& session) {
    check_cancelled(session);
    if (certificate_trusted())
        return;
    const auto executable = executable_path();
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpParameters = L"--trust-certificate";
    launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch))
        winrt::throw_last_error();
    winrt::handle process(launch.hProcess);
    if (WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0)
        winrt::throw_last_error();
    DWORD exit{};
    if (!GetExitCodeProcess(process.get(), &exit))
        winrt::throw_last_error();
    if (exit != ERROR_SUCCESS || !certificate_trusted())
        throw winrt::hresult_error(E_ACCESSDENIED, L"证书准备未完成。请允许 Windows 的管理员确认后重试。");
    check_cancelled(session);
}

void prepare_dependencies(Session& session, Progress const& progress) {
    using namespace winrt::Windows::Management::Deployment;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct ApartmentScope {
        ~ApartmentScope() {
            winrt::uninit_apartment();
        }
    } apartment;
    PackageManager manager;
    for (const auto& dependency : payload::dependencies) {
        check_cancelled(session);
        bool installed{};
        for (const auto& package : manager.FindPackagesForUser(L"", dependency.name, payload::microsoft_publisher)) {
            const auto id = package.Id();
            const auto version = id.Version();
            const auto number = (static_cast<std::uint64_t>(version.Major) << 48) |
                                (static_cast<std::uint64_t>(version.Minor) << 32) |
                                (static_cast<std::uint64_t>(version.Build) << 16) | version.Revision;
            if (number >= dependency.version &&
                id.Architecture() == winrt::Windows::System::ProcessorArchitecture::X64 &&
                package.Status().VerifyIsOK()) {
                installed = true;
                break;
            }
        }
        if (installed)
            continue;
        progress(L"正在下载 " + std::wstring(dependency.label) + L"…");
        const auto file = session.directory / dependency.file;
        download(session, dependency.url, dependency.sha256, file);
        check_cancelled(session);
        progress(L"正在准备 " + std::wstring(dependency.label) + L"…");
        const auto deployment =
            manager.AddPackageAsync(winrt::Windows::Foundation::Uri(file.wstring()), nullptr, DeploymentOptions::None);
        // Cancellation only affects this deployment; no force-close or downgrade flags.
        while (deployment.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
            if (session.cancelled.load(std::memory_order_relaxed))
                deployment.Cancel();
            Sleep(50);
        }
        const auto result = deployment.get();
        if (FAILED(result.ExtendedErrorCode()))
            throw winrt::hresult_error(result.ExtendedErrorCode(), result.ErrorText());
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
    }
}

bool app_installer_available() {
    winrt::Windows::Management::Deployment::PackageManager manager;
    for (const auto& package :
         manager.FindPackagesForUser(L"", L"Microsoft.DesktopAppInstaller", payload::microsoft_publisher))
        if (package.Status().VerifyIsOK())
            return true;
    return false;
}

void open_app_installer(Session& session) {
    check_cancelled(session);
    if (!app_installer_available())
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_NOT_FOUND),
                                   L"请先从 Microsoft Store 安装“应用安装程序”，然后重试。");
    const auto feed = session.directory / L"Unfurl.appinstaller";
    if (!std::filesystem::exists(feed))
        save_resource(12, feed);
    const auto result = ShellExecuteW(nullptr, L"open", feed.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
        throw winrt::hresult_error(E_FAIL, L"Windows 无法打开安装确认窗口，请检查“应用安装程序”。");
    session.keep_feed = true;
}

} // namespace unfurl::setup
