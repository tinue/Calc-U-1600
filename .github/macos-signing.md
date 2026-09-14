# macOS signing & notarization

The `macos` job in [`workflows/build.yml`](workflows/build.yml):

1. Runs `macdeployqt` on the `.app` (Qt6/CMakeLists.txt's `MACOSX_BUNDLE`) to embed the
   Qt frameworks, so the bundle doesn't depend on Homebrew's Qt being present on the
   machine that runs it.
2. Developer-ID-signs the `.app` with the **hardened runtime**.
3. Notarizes and staples the `.app`.
4. Packages it into a `.dmg` (with an `/Applications` symlink for drag-install), signs,
   notarizes, and staples the `.dmg` too.

Same Apple Developer ID / Team ID (`7Q3QN9AV9J`) as SharpDataExchangeRust -- see that
repo's `.github/macos-signing.md` for the original cert/key setup. Bundle identifier
here is `ch.erzberger.CalcU1600Qt` (set in `Qt6/CMakeLists.txt`).

Unlike SharpDataExchangeRust, this workflow only uses the **Developer ID Application**
cert -- a `.dmg` can be signed with it directly, no *Developer ID Installer* cert or
`.pkg` needed. The `APPLE_CERT_INSTALLER_P12_BASE64` secret is still set (reused from
the same six secrets) but currently unused here.

## Repo secrets

Six secrets, same names/contents as SharpDataExchangeRust's:

| Secret | Contents |
|---|---|
| `APPLE_CERT_APPLICATION_P12_BASE64` | base64 of the `Developer ID Application` cert's `.p12` |
| `APPLE_CERT_INSTALLER_P12_BASE64`   | base64 of the `Developer ID Installer` cert's `.p12` (unused here, kept for parity) |
| `APPLE_CERT_P12_PASSWORD`           | the `.p12` export password (same for both) |
| `APPLE_API_KEY_P8_BASE64`           | base64 of the App Store Connect API key (`AuthKey_XXXXXXXXXX.p8`) |
| `APPLE_API_KEY_ID`                  | that key's ID |
| `APPLE_API_ISSUER_ID`               | the issuer UUID |

Since it's the same Apple Developer ID, these can be re-exported from the same Mac/
Keychain that already has them (both cert private keys) rather than generating new
certs -- see SharpDataExchangeRust's `.github/macos-signing.md` for the export steps.
The App Store Connect API key (`.p8`) is a **one-time Apple download** -- if the
original file is lost, generate a new key rather than trying to recover it.

```sh
gh secret set APPLE_CERT_APPLICATION_P12_BASE64 < <(base64 -i DeveloperIDApplication.p12)
gh secret set APPLE_CERT_INSTALLER_P12_BASE64   < <(base64 -i DeveloperIDInstaller.p12)
gh secret set APPLE_API_KEY_P8_BASE64           < <(base64 -i AuthKey_XXXXXXXXXX.p8)
printf %s 'THE_EXPORT_PASSWORD'                  | gh secret set APPLE_CERT_P12_PASSWORD
printf %s 'ABCDE12345'                           | gh secret set APPLE_API_KEY_ID
printf %s '69a6de7f-1a2b-3c4d-5e6f-70a1b2c3d4e5' | gh secret set APPLE_API_ISSUER_ID
```

## Rotating

Certs expire every ~5 years. Re-export the `.p12`, re-run the two
`gh secret set … _P12_BASE64` commands. For a new API key, replace the `.p8` secret
plus `APPLE_API_KEY_ID` / `APPLE_API_ISSUER_ID`.
