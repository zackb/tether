// Background worker script
// This runs in the background of the browser or mail client

// Import the native messaging module if supported (MV3 modules)
// For MV2 (Thunderbird), we might need to load this differently or bundle it.
import { connectToNativeHost, sendToNativeHost } from '../shared/native.js';

let nativePort = connectToNativeHost();

// Listen for messages from content scripts or the mail extractor
if (typeof chrome !== 'undefined' && chrome.runtime) {
  chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
    if (request.action === "found_otp_in_email") {
      console.log("Found OTP in email, sending to Tether daemon:", request.otp);
      // Send the extracted OTP to the C++ daemon
      sendToNativeHost({
        type: "new_otp",
        otp: request.otp,
        source: request.source
      });
      sendResponse({ status: "sent_to_daemon" });
    }
    
    if (request.action === "request_otp_for_site") {
      console.log("Content script requesting OTP for:", request.url);
      // Query the C++ daemon for an OTP
      sendToNativeHost({
        type: "request_otp",
        url: request.url
      });
      sendResponse({ status: "requested" });
    }
    return true;
  });
}
