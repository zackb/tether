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
