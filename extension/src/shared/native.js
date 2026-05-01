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

  port.onDisconnect.addListener(() => {
    console.log("Disconnected from Tether daemon");
    port = null;
    // Implement reconnection logic if needed
  });

  return port;
}

export function sendToNativeHost(message) {
  if (port) {
    port.postMessage(message);
  } else {
    console.error("Not connected to Tether daemon");
  }
}

function handleNativeMessage(message) {
  // Dispatch native messages to other parts of the extension
  if (message.type === "otp_available") {
    // Notify content scripts or popup
    if (typeof chrome !== 'undefined' && chrome.tabs) {
      chrome.tabs.query({active: true, currentWindow: true}, function(tabs) {
        if (tabs[0]) {
          chrome.tabs.sendMessage(tabs[0].id, { action: "fill_otp", otp: message.otp });
        }
      });
    }
  }
}
