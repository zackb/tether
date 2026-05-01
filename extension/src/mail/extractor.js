// Mail script for Thunderbird/Betterbird
// This script extracts OTP codes from emails

// This example assumes Manifest V2/V3 background script using the WebExtension Mail APIs
// (messenger.messages, messenger.mailTabs)

import { sendToNativeHost } from '../shared/native.js';

if (typeof messenger !== 'undefined') {
  console.log("Tether Mail Extractor loaded in Thunderbird/Betterbird");

  // Listen for new messages arriving in the background completely silently
  if (messenger.messages.onNewMailReceived) {
    messenger.messages.onNewMailReceived.addListener(async (folder, messages) => {
      // messages is a MessageList, we need to iterate its messages array
      for (const message of messages.messages) {
        processMessage(message);
      }
    });
  }

  // Also process manually opened messages just in case it missed it
  messenger.messageDisplay.onMessageDisplayed.addListener(async (tabId, message) => {
    processMessage(message);
  });

  async function processMessage(message) {
    // Get the full message body
    const full = await messenger.messages.getFull(message.id);
    const bodyText = extractTextFromParts(full.parts);
    
    // Simple regex to find 6-8 digit OTP codes
    const otpRegex = /\b\d{6,8}\b/g;
    const matches = bodyText.match(otpRegex);
    
    if (matches && matches.length > 0) {
      const otp = matches[0]; // Simplistic approach: take the first one
      console.log("Found OTP in email:", otp);
      
      // Send it directly to the native daemon
      sendToNativeHost({
        command: "new_otp",
        otp: otp,
        source: message.subject
      });
    }
  }

  function extractTextFromParts(parts) {
    let text = "";
    if (!parts) return text;
    for (const part of parts) {
      if (part.contentType === "text/plain" && part.body) {
        text += part.body + "\n";
      } else if (part.parts) {
        text += extractTextFromParts(part.parts);
      }
    }
    return text;
  }
}
