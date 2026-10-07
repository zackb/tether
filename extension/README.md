# Tether Extension

This is the cross-platform WebExtension for Tether. The extension spans two different environments with distinct permission requirements:
1. **Mail Clients (Thunderbird/Betterbird)**: Extracts OTPs from emails.
2. **Web Browsers (Chrome/Firefox)**: Autofills extracted OTPs on webpages.

Both extensions communicate with the local `tetherd` native daemon via standard Native Messaging APIs.

## Directory Structure
- `manifest-browser.json`: The Manifest V3 definition for Chrome/Firefox browsers.
- `manifest-mail.json`: The Manifest V2/V3 definition for Thunderbird/Betterbird.
- `src/background/`: Service worker / background scripts handling native messaging and state.
- `src/content/`: Scripts injected into web pages to locate OTP input fields and fill them.
- `src/mail/`: Thunderbird-specific scripts utilizing `messenger.*` APIs to parse emails for OTP codes.
- `src/shared/`: Code shared across both environments (e.g., Native Messaging wrappers).
- `host/`: Contains the Native Messaging Host JSON definition needed to register `tetherd` with browsers.

## How it Works
1. **In Thunderbird**: The extension monitors incoming emails or displayed messages. `src/mail/extractor.js` parses the text for an OTP and sends it through `src/shared/native.js` directly to the `tetherd` daemon.
2. **The Daemon (`tetherd`)**: Acts as a state manager and vault.
3. **In the Browser**: When you navigate to a page asking for an OTP, `src/content/autofill.js` recognizes the field and requests the OTP from `tetherd`. The daemon returns the OTP, which is then injected into the DOM.

## Installation (Native Messaging Host)

To allow the extension to talk to the local `tether` binary, you must register the native messaging host manifest.

### 1. Locate the manifests
After building the project, the manifests are generated in your build directory:
- `build/com.tether.extension.chrome.json`
- `build/com.tether.extension.mozilla.json`

### 2. Create Symlinks
**Important:** Most browsers (especially Firefox) require the manifest filename in the config directory to **exactly match** the `"name"` field inside the JSON (`com.tether.extension`).

#### Firefox / Thunderbird
```bash
mkdir -p ~/.mozilla/native-messaging-hosts/
ln -s $(pwd)/build/com.tether.extension.mozilla.json ~/.mozilla/native-messaging-hosts/com.tether.extension.json
```

#### Chromium / Google Chrome
1. Find your Extension ID in `chrome://extensions`.
2. Ensure it is listed in the `allowed_origins` field of the manifest.
3. Link the file:
```bash
mkdir -p ~/.config/chromium/NativeMessagingHosts/
ln -s $(pwd)/build/com.tether.extension.chrome.json ~/.config/chromium/NativeMessagingHosts/com.tether.extension.json
```


## Local Chrome / Chromium installation

From the repository root, run `bash extension/build.sh` (Node.js, npm and zip
are required). The Chrome build is written to `build/extension/chromium` and
archived as `build/extension/tether-chromium-extension.zip`.

1. Open `chrome://extensions` in the desired browser profile.
2. Enable **Developer mode**, click **Load unpacked**, and select the absolute
   path to `build/extension/chromium` in this checkout.
3. Copy the extension ID displayed by the browser.
4. With Tether installed, register the native host:

   ```sh
   python3 extension/host/install-chromium-host.py EXTENSION_ID
   # For Chromium instead of Google Chrome:
   python3 extension/host/install-chromium-host.py EXTENSION_ID --browser chromium
   ```

The helper finds `tether-native-host` on PATH. Use `--host /absolute/path` if
installed elsewhere. It backs up an existing user registration and preserves
its allowed extension origins. No sudo is needed. Reload the extension after
registration. Tether's daemon must be running for OTP autofill.

Load the extension separately in each desired profile. Keep the checkout at
its current path, rebuild after updating it, then click **Reload** in the
extensions page. This is an unpacked local installation, not a signed CRX or
Chrome Web Store release. The extension ID can change if its path changes;
rerun the helper with the new ID when necessary.
