// Native messaging connection logic
let port = null;

export function connectToNativeHost() {
  const hostName = "com.tether.extension";
  if (typeof browser !== 'undefined' && browser.runtime && browser.runtime.connectNative) {
    port = browser.runtime.connectNative(hostName);
  } else if (typeof chrome !== 'undefined' && chrome.runtime && chrome.runtime.connectNative) {
    port = chrome.runtime.connectNative(hostName);
  } else {
    console.error("Native messaging not supported in this environment");
    return null;
  }

  port.onMessage.addListener((message) => {
    console.log("Received message from Tether daemon:", message);
    handleNativeMessage(message);
  });

  port.onDisconnect.addListener((p) => {
    let errorMsg = "unknown reason";
    if (p.error) {
        errorMsg = p.error.message;
    } else if (typeof browser !== 'undefined' && browser.runtime.lastError) {
      errorMsg = browser.runtime.lastError.message;
    } else if (typeof chrome !== 'undefined' && chrome.runtime.lastError) {
      errorMsg = chrome.runtime.lastError.message;
    }
    console.log("Disconnected from Tether daemon. Reason:", errorMsg);
    port = null;
  });

  return port;
}

export function sendToNativeHost(message) {
  // Under MV3 the service worker (and its native host) can be torn down between
  // sends; lazily re-establish the port so messages and the subscription survive.
  if (!port) {
    connectToNativeHost();
  }
  if (port) {
    port.postMessage(message);
  } else {
    console.error("Not connected to Tether daemon");
  }
}

function handleNativeMessage(message) {
  // Dispatch native messages to other parts of the extension
  if (message.command === "otp_available" && message.otp) {
    // Deliver to every tab; the content script only fills when the page actually
    // has OTP fields, so we don't depend on which tab happens to be focused.
    if (typeof chrome !== 'undefined' && chrome.tabs) {
      chrome.tabs.query({}, function(tabs) {
        for (const tab of tabs) {
          chrome.tabs.sendMessage(tab.id, { action: "fill_otp", otp: message.otp }, () => {
            // Ignore tabs without a content script listening.
            void chrome.runtime.lastError;
          });
        }
      });
    }
  }
}
