# Installation, updates, and releases

Unfurl uses Microsoft's signed MSIX + App Installer deployment model on Windows 11 x64. Both the installer feed and its packages are hosted in public [GitHub Releases](https://github.com/L0stInFades/Unfurl/releases).

## Install

Download [UnfurlSetup.exe](https://github.com/L0stInFades/Unfurl/releases/latest/download/UnfurlSetup.exe) and open it normally. This standalone WinUI 3 application embeds the public certificate, the App Installer feed, and Microsoft's signed bootstrap DLL. Choose **准备并继续**. It requests elevation for the fixed certificate import when needed, prepares missing Microsoft runtime components, then opens Windows App Installer for the final **Install** confirmation. No adjacent files or manual administrator launch are required. The signed executable is under 1 MiB; runtime packages are downloaded only when missing.

If WinUI is not yet available, a native Windows preparation dialog installs its prerequisites before showing the WinUI page. Downloads use HTTPS and verify both the pinned SHA-256 and package signature. Suitable installed frameworks are reused. Cancellation stops preparation and removes temporary payloads; successfully deployed shared frameworks remain installed. After handoff, only the small feed is retained in the temporary directory because App Installer reads it asynchronously. Windows App Installer must be available from Microsoft Store.

For manual installation, the certificate/feed flow below remains available.

The initial release uses a project-specific self-signed code-signing certificate. Windows does not trust it automatically. Microsoft recommends a certificate from a trusted issuer for broad distribution; this release requires the following explicit, one-time trust step.

1. Download `Unfurl.cer` and `Unfurl.appinstaller` from the latest release.
2. Open an administrator PowerShell in the download directory and verify/import the public certificate:

   ```powershell
   $certificate = Get-PfxCertificate -FilePath ./Unfurl.cer
   if ($certificate.Thumbprint -ne '7D6647D7B3568C1517146B098A55488ADF5A4BA1') {
       throw 'Unexpected Unfurl signing certificate.'
   }
   Import-Certificate -FilePath ./Unfurl.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
   ```

3. Double-click `Unfurl.appinstaller` and choose Install. App Installer downloads the signed app and the Microsoft-signed Windows App Runtime and Visual C++ framework dependencies. Start Unfurl from the Start menu.

Install through **the `.appinstaller` file** to enroll in automatic updates. Installing the `.msix` alone does not enroll the application in this feed. The portable ZIP also does not automatically update. The Windows App Installer application must be installed; organizational App Installer policies and network restrictions can disable or prevent updates.

The certificate contains only the public key. Its SHA-1 thumbprint above identifies the certificate; release assets are separately hashed with SHA-256 in `SHA256SUMS.txt`. No private key is shipped. Do not import the certificate into Trusted Root Certification Authorities.

## Update behavior

The stable feed address is:

```text
https://github.com/L0stInFades/Unfurl/releases/latest/download/Unfurl.appinstaller
```

`OnLaunch HoursBetweenUpdateChecks="0"` asks Windows to check on every app launch. `AutomaticBackgroundTask` asks Windows to check every eight hours even when the app is not running. Updates are silent for packaged desktop apps and are applied when Windows can service the package. Unfurl does not force-close an archive operation or block activation for an update. The eight-hour schedule is managed by Windows and is not an immediate delivery guarantee.

Each feed points to immutable, version-specific package URLs under `/releases/download/vX.Y.Z/`. New releases keep the package `Name` (`L0stInFades.Unfurl`), `Publisher` (`CN=L0stInFades`), architecture, and signing certificate, and increase the four-part MSIX version. The feed version increases with it. Downgrades are disabled. Start from the Start menu when checking launch-based updates; direct executable launches are not a substitute for testing Windows package activation.

To manually request an update, download and open the latest `Unfurl.appinstaller` again, then close Unfurl when convenient. Uninstall through Windows Settings > Apps > Installed apps. Uninstalling the app does not remove the certificate from Trusted People.

## Build and publish

Run in a Visual Studio Developer PowerShell with PowerShell 7, the Windows SDK, `gh`, and the restored dependencies. The release is signed locally because this machine holds the non-exportable private key. Keep this Windows user profile and its key store available for subsequent releases. GitHub Actions validation certificates must never replace this release identity.

```powershell
./eng/new-signing-certificate.ps1
./eng/package-msix.ps1 -CertificateThumbprint 7D6647D7B3568C1517146B098A55488ADF5A4BA1
./eng/verify-release.ps1
ctest --test-dir build/windows-release --output-on-failure --timeout 30
```

The certificate creation script reuses the existing unexpired certificate. It never exports the private key. `package-msix.ps1` reuses the portable packaging pipeline, generates PNG logos from the app icon, validates Microsoft framework signatures, uses MakeAppx validation, and signs and timestamps the MSIX using SignTool. It invokes `package-setup.ps1` to generate the embedded feed, certificate, and pinned dependency metadata, build the installer separately, and sign and timestamp it with the same identity. The MSIX version defaults to the executable's four-part file version. Update `CMakeLists.txt`, `src/app/Unfurl.rc`, and the installer version resources for a new app version before building.

`artifacts/release/release.json` lists exactly which public assets to upload. Stage every asset in a draft release before publishing it so the `latest` feed never points to missing assets. Do not replace assets of a published version; build a higher version instead. Every stable release in this repository must include `Unfurl.appinstaller` because GitHub's `latest` redirect is the update channel. Do not mark a prerelease as latest.

```powershell
$release = Get-Content ./artifacts/release/release.json -Raw | ConvertFrom-Json
git tag $release.Tag
git push origin main $release.Tag
$assets = @($release.Assets | ForEach-Object { Join-Path ./artifacts/release $_ })
gh release create $release.Tag @assets --repo $release.Repository --verify-tag --draft `
    --title "Unfurl $($release.Tag)" --notes-file "./docs/releases/$($release.Tag).md"
gh release view $release.Tag --repo $release.Repository --json assets,isDraft,targetCommitish
gh release edit $release.Tag --repo $release.Repository --draft=false --latest
```

Before tagging, commit the verified source and use release notes for that version. After publishing, fetch the public latest feed and every referenced asset without authentication, compare hashes, and install through the public feed. A private repository's authenticated assets cannot serve as a Windows App Installer update source.

## Microsoft references

- [Create an App Installer file manually](https://learn.microsoft.com/en-us/windows/msix/app-installer/how-to-create-appinstaller-file)
- [Configure update settings](https://learn.microsoft.com/en-us/windows/msix/app-installer/update-settings)
- [OnLaunch, including desktop-app limitations](https://learn.microsoft.com/en-us/uwp/schemas/appinstallerschema/element-onlaunch)
- [Create a package-signing certificate and trust it](https://learn.microsoft.com/en-us/windows/msix/package/create-certificate-package-signing)
- [Sign an app package using SignTool](https://learn.microsoft.com/en-us/windows/msix/package/sign-app-package-using-signtool)
- [Windows App SDK deployment for packaged apps](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/deploy-packaged-apps)
- [Visual C++ framework packages for Desktop Bridge](https://learn.microsoft.com/en-us/troubleshoot/developer/visualstudio/cpp/libraries/c-runtime-packages-desktop-bridge)
