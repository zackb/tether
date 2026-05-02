// Mail script for Thunderbird/Betterbird
// This script extracts OTP codes from emails

// This example assumes Manifest V2/V3 background script using the WebExtension Mail APIs
// (messenger.messages, messenger.mailTabs)

import { connectToNativeHost, sendToNativeHost } from '../shared/native.js';

connectToNativeHost();

const OTP_CONTEXT_KEYWORDS = [
  'code', 'otp', 'one-time', 'verification', 'confirm', 'passcode',
  'authenticate', 'login', 'sign in', '2fa', 'token', 'pin',
  'expires', 'valid for', 'do not share', 'use this'
];

const OTP_PATTERNS = [
  /\b(\d{4,8})\b/g,                         // plain digits 4–8 long
  /\b([A-Z0-9]{6,10})\b/g,                  // alphanumeric (some services)
  /\b(\d{3}[-\s]\d{3})\b/g,                 // formatted: 123 456 or 123-456
];

const OTP_SUBJECT_PATTERNS = [
  /verification/i, /your .{0,15} code/i, /one.time/i,
  /confirm.{0,10}(email|account)/i, /sign.in code/i, /login code/i,
  /\bOTP\b/i, /security code/i, /\d{4,8} is your/i
];

function scoreCandidate(num, surroundingText) {
  let score = 0;
  const lower = surroundingText.toLowerCase();

  const numIndex = lower.indexOf(num.toLowerCase());

  for (const kw of OTP_CONTEXT_KEYWORDS) {
    if (lower.includes(kw)) {
      score += 10;

      // Proximity bonus: keyword within 20 chars of the number
      if (numIndex !== -1) {
        const kwIndex = lower.indexOf(kw);
        if (Math.abs(numIndex - kwIndex) <= 30) {
          score += 20;
        }
      }
    }
  }
  return score;
}

function isFalsePositive(num, context) {
  if (/^(19|20)\d{2}$/.test(num)) return true;  // year
  if (num.startsWith('0') && num.length > 6) return true;  // phone-like
  if (context.includes('$') || context.includes('USD')) return true;  // price

  const lower = context.toLowerCase();
  if (lower.includes('order') || lower.includes('tracking')) return true;
  return false;
}

if (typeof messenger !== 'undefined') {
  console.log("Tether Mail Extractor loaded in Thunderbird/Betterbird");

  // Track message IDs we've already processed to avoid duplicates
  const seenMessageIds = new Set();

  async function processNewMessagesInFolder(folder) {
    try {
      const page = await messenger.messages.list(folder);
      for (const message of page.messages) {
        if (!seenMessageIds.has(message.id)) {
          seenMessageIds.add(message.id);
          processMessage(message);
        }
      }
    } catch (e) {
      // Folder may not support listing (e.g. virtual folders)
    }
  }

  // Tier 1: onNewMailReceived — works for POP3 / local delivery / filters
  if (messenger.messages.onNewMailReceived) {
    messenger.messages.onNewMailReceived.addListener(async (folder, messages) => {
      for (const message of messages.messages) {
        if (!seenMessageIds.has(message.id)) {
          seenMessageIds.add(message.id);
          processMessage(message);
        }
      }
    });
  }

  // Tier 2: onFolderInfoChanged — catches IMAP synced mail that onNewMailReceived misses
  // Fires when folder unread/total counts change, i.e. new messages landed via IMAP
  if (messenger.folders?.onFolderInfoChanged) {
    messenger.folders.onFolderInfoChanged.addListener(async (folder, folderInfo) => {
      // Only scan inbox-type folders where new mail typically lands
      if (folder.type === 'inbox' || folder.type === 'other') {
        await processNewMessagesInFolder(folder);
      }
    });
  }

  // Tier 3: onMessageDisplayed — final fallback when opening an email manually
  messenger.messageDisplay.onMessageDisplayed.addListener(async (tabId, message) => {
    if (!seenMessageIds.has(message.id)) {
      seenMessageIds.add(message.id);
      processMessage(message);
    }
  });


  async function processMessage(message) {
    const full = await messenger.messages.getFull(message.id);
    const bodyText = extractTextFromParts(full.parts);
    
    // Email Subject Line as a Strong Prior
    let subjectMatches = false;
    for (const pattern of OTP_SUBJECT_PATTERNS) {
      if (pattern.test(message.subject)) {
        subjectMatches = true;
        break;
      }
    }

    let candidates = [];
    
    for (const pattern of OTP_PATTERNS) {
      let match;
      const regex = new RegExp(pattern.source, pattern.flags);

      while ((match = regex.exec(bodyText)) !== null) {
        const num = match[1] || match[0];

        // Skip purely alphabetical matches
        if (/^[A-Za-z]+$/.test(num)) continue;

        const startIndex = Math.max(0, match.index - 50);
        const endIndex = Math.min(bodyText.length, match.index + match[0].length + 50);
        const context = bodyText.substring(startIndex, endIndex);

        if (isFalsePositive(num, context)) continue;

        let score = scoreCandidate(num, context);
        if (subjectMatches) score += 30;

        candidates.push({ num, score });
      }
    }

    if (candidates.length > 0) {
      // Sort by score descending
      candidates.sort((a, b) => b.score - a.score);
      const best = candidates[0];

      if (best.score > 0) {
        console.log("Found OTP candidate in email:", best.num, "with score:", best.score);
        sendToNativeHost({
          command: "new_otp",
          otp: best.num.replace(/[-\s]/g, ''),
          source: message.subject
        });
      }
    }
  }

  function extractTextFromParts(parts) {
    let text = "";
    if (!parts) return text;
    for (const part of parts) {
      if (part.contentType === "text/plain" && part.body) {
        text += part.body + "\n";
      } else if (part.contentType === "text/html" && part.body) {
        let htmlText = "";
        try {
          const parser = new DOMParser();
          const doc = parser.parseFromString(part.body, 'text/html');
          const candidates = doc.querySelectorAll('strong, b, h1, h2, td, .otp, .code');
          for (const el of candidates) {
            htmlText += el.textContent + " \n ";
          }
        } catch (e) {
          console.error("DOMParser error", e);
        }

        let htmlBody = part.body;
        htmlBody = htmlBody.replace(/<style[^>]*>[\s\S]*?<\/style>/gi, ' ');
        htmlBody = htmlBody.replace(/<script[^>]*>[\s\S]*?<\/script>/gi, ' ');
        htmlBody = htmlBody.replace(/<[^>]+>/g, ' ');

        text += htmlText + "\n" + htmlBody + "\n";
      } else if (part.parts) {
        text += extractTextFromParts(part.parts);
      }
    }
    return text;
  }
}

