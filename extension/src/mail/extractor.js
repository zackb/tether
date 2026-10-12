// Mail script for Thunderbird/Betterbird
// This script extracts OTP codes from emails

// single entry point for the mail extension
import '../background/background.js';
import { sendToNativeHost, registrableDomain } from '../shared/native.js';

const OTP_CONTEXT_KEYWORDS = [
  'code', 'otp', 'one-time', 'verification', 'confirm', 'passcode',
  'authenticate', 'login', 'sign in', '2fa', 'token', 'pin',
  'expires', 'valid for', 'do not share', 'use this'
];

export const OTP_PATTERNS = [
  /\b(\d{4,8})\b/g,                         // plain digits 4–8 long
  /\b([A-Z0-9]{6,10})\b/g,                  // alphanumeric (some services)
  /\b(\d{3}[-\s]\d{3})\b/g,                 // formatted: 123 456 or 123-456
];

const OTP_SUBJECT_PATTERNS = [
  /verification/i, /your .{0,15} code/i, /one.time/i,
  /confirm.{0,10}(email|account)/i, /sign.in code/i, /login code/i,
  /\bOTP\b/i, /security code/i, /\d{4,8} is your/i
];

const OTP_TTL_MS = 10 * 60 * 1000; // only look at emails from the last 10 minutes

// distance from `target` to the closest occurrence of `needle` in `haystack`.
function nearestDistance(haystack, needle, target) {
  let best = Infinity;
  for (let i = haystack.indexOf(needle); i !== -1; i = haystack.indexOf(needle, i + 1)) {
    const d = Math.abs(target - i);
    if (d < best) best = d;
    if (i > target) break; // occurrences are ordered; past the target we only get worse
  }
  return best;
}

export function scoreCandidate(num, surroundingText) {
  let score = 0;
  // Squash whitespace for accurate proximity and keyword matching
  const cleanText = surroundingText.replace(/\s+/g, ' ').toLowerCase();
  const lower = cleanText;
  const numIndex = lower.indexOf(num.toLowerCase());

  for (const kw of OTP_CONTEXT_KEYWORDS) {
    if (lower.indexOf(kw) === -1) continue;
    score += 10;

    // proximity bonus: keyword within 30 chars of the number
    if (numIndex !== -1 && nearestDistance(lower, kw, numIndex) <= 30) {
      score += 20;
    }
  }

  // Bonus if the match was found inside a visually prominent element
  if (surroundingText.includes('PROMINENT:')) score += 25;

  return score;
}

export function isFalsePositive(num, context) {
  if (/^(19|20)\d{2}$/.test(num)) return true;  // year
  if (num.startsWith('0') && num.length > 6) return true;  // phone-like
  if (context.includes('$') || context.includes('USD')) return true;  // price

  // Zip codes (State + 5 digits)
  if (new RegExp('\\b[A-Z]{2}\\s+' + num + '\\b').test(context)) return true;

  const lower = context.toLowerCase();
  if (lower.includes('order') || lower.includes('tracking')) return true;
  return false;
}

// Pull the registrable domain out of a From header such as
// "Amazon <noreply@amazon.com>" or a bare "no-reply@accounts.google.com".
export function extractSenderDomain(author) {
  if (!author || typeof author !== 'string') return '';
  const match = author.match(/[\w.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9.-]+/);
  if (!match) return '';
  const domain = match[0].split('@')[1];
  return domain.toLowerCase().replace(/\.+$/, '');
}

function normalizeDomainValue(d) {
  return String(d || '').toLowerCase().replace(/\.+$/, '').replace(/"/g, '');
}

// Only trust domains the receiving server actually validated. Raw From/Return-Path
// and even a raw DKIM-Signature header are attacker-controlled; the server's own
// Authentication-Results header records what really passed. We only look at the
// first (topmost) header — the one added by the user's receiving server.
function validatedDkimDomains(headers) {
  const raw = headers?.['authentication-results'] || [];
  if (!raw.length) return [];
  const domains = [];
  for (const part of String(raw[0]).split(';')) {
    if (!/dkim\s*=\s*pass/i.test(part)) continue;

    let m = part.match(/header\.d\s*=\s*"?([^";\s]+)"?/i);
    if (m) {
      const d = normalizeDomainValue(m[1]);
      if (d) {
        domains.push(d);
        continue;
      }
    }

    m = part.match(/header\.i\s*=\s*"?([^";\s]+)"?/i);
    if (m) {
      const addr = m[1];
      const at = addr.indexOf('@');
      const d = normalizeDomainValue(at >= 0 ? addr.slice(at + 1) : addr);
      if (d) domains.push(d);
    }
  }
  return domains;
}

// Some services send transactional mail (including OTPs) from a registrable
// domain that differs from the site the user logs in on. Map those sender
// domains to the login domain so the correlation still matches.
const SENDER_DOMAIN_ALIASES = new Map([
  // Facebook sends security/verification mail from facebookmail.com.
  ['facebookmail.com', 'facebook.com'],
]);

export function canonicalizeSenderDomain(domain) {
  const rd = registrableDomain(domain);
  return SENDER_DOMAIN_ALIASES.get(rd) || rd;
}

// Derive the sender domain solely from the server-validated DKIM result. When
// the receiving server did not record a passing DKIM signature, return "" so the
// OTP is left unpinned instead of trusting a spoofable From/Return-Path header.
export function resolveSenderDomain(message, headers) {
  const from = extractSenderDomain(message?.author);
  const validated = validatedDkimDomains(headers);
  if (validated.length > 0) {
    // Prefer the validated domain aligned with the From header, otherwise the
    // first one the receiving server reported.
    const domain = validated.find((d) => registrableDomain(d) === registrableDomain(from)) || validated[0];
    return canonicalizeSenderDomain(domain);
  }
  return '';
}

// Fetch the raw headers and derive the best sender domain. getFull() is the
// only API that exposes headers; it is only called once an OTP candidate has
// been found, so ordinary (non-OTP) mail never pays for it.
async function getSenderDomain(message) {
  try {
    const full = await messenger.messages.getFull(message.id);
    return resolveSenderDomain(message, full.headers);
  } catch (e) {
    return extractSenderDomain(message.author);
  }
}

// ---------------------------------------------------------------------------
// FALLBACK text extraction: manually walk getFull() parts.
// Recovery path for messages whose MIME tree makes listInlineTextParts() throw.
// For HTML parts we use DOMParser to tag prominent elements for the scorer,
// then grab the full body plain text.
// ---------------------------------------------------------------------------
function extractTextFromParts(parts) {
  let text = "";
  if (!parts) return text;

  for (const part of parts) {
    if (part.contentType === "text/plain" && part.body) {
      text += part.body + "\n";

    } else if (part.contentType === "text/html" && part.body) {
      try {
        const parser = new DOMParser();
        const doc = parser.parseFromString(part.body, 'text/html');

        // Remove noise nodes before any extraction
        doc.querySelectorAll('style, script, footer, nav').forEach(el => el.remove());

        // Tag visually prominent elements so the scorer can give them a bonus
        const prominentParts = [];
        doc.querySelectorAll(
          'strong, b, h1, h2, h3, .otp, .code, [class*="otp"], [class*="code"], [class*="verif"]'
        ).forEach(el => {
          const t = el.textContent.trim();
          if (t) prominentParts.push('PROMINENT:' + t);
        });

        // Also tag elements that likely contain only the code
        doc.querySelectorAll('td, th, span, div, p, font').forEach(el => {
          const t = el.textContent.trim();
          if (/^\d{4,8}$/.test(t) || /^[A-Z0-9]{6,10}$/.test(t)) {
            prominentParts.push('PROMINENT:' + t);
          }
        });

        const bodyPlain = doc.body?.textContent || '';
        if (prominentParts.length > 0) {
          // Pad prominent parts to avoid false proximity
          text += prominentParts.join('\n' + ' '.repeat(50) + '\n') + '\n' + ' '.repeat(50) + '\n';
        }
        text += bodyPlain + '\n';
      } catch (e) {
        console.error("DOMParser error", e);
      }

    } else if (part.parts) {
      text += extractTextFromParts(part.parts);
    }
  }

  return text;
}

// ---------------------------------------------------------------------------
// PRIMARY text extraction. Both APIs set the manifest's strict_min_version:
//
//   messenger.messages.listInlineTextParts(id)            - TB 128+
//     - gives us clean content without manually walking the MIME tree
//
//   messenger.messengerUtilities.convertToPlainText(html) - TB 137+
//     - Thunderbird's own HTML-to-plaintext converter; handles tables,
//       encoded entities, and nested elements far better than a hand-rolled
//       DOMParser tag-strip. Borrowed from thunderbird-vericode's approach.
//
// We still run the DOMParser tagging pass on the raw HTML before
// converting, so the scorer's +25 bonus is preserved on this code path too.
//
// extractTextFromParts() catches runtime failures, not missing APIs.
// ---------------------------------------------------------------------------
async function getEmailText(messageId) {
  try {
    const parts = await messenger.messages.listInlineTextParts(messageId);
    let text = '';

    for (const part of parts) {
      if (part.contentType === 'text/plain') {
        text += part.content + '\n';

      } else if (part.contentType === 'text/html') {
        // tagging pass on the raw HTML first, before conversion
        // strips all tags. This preserves our scoring bonus.
        try {
          const parser = new DOMParser();
          const doc = parser.parseFromString(part.content, 'text/html');
          doc.querySelectorAll('style, script, footer, nav').forEach(el => el.remove());

          const prominentParts = [];
          doc.querySelectorAll(
            'strong, b, h1, h2, h3, .otp, .code, [class*="otp"], [class*="code"], [class*="verif"]'
          ).forEach(el => {
            const t = el.textContent.trim();
            if (t) prominentParts.push('PROMINENT:' + t);
          });

          // Also tag elements that likely contain only the code
          doc.querySelectorAll('td, th, span, div, p, font').forEach(el => {
            const t = el.textContent.trim();
            if (/^\d{4,8}$/.test(t) || /^[A-Z0-9]{6,10}$/.test(t)) {
              prominentParts.push('PROMINENT:' + t);
            }
          });

          if (prominentParts.length > 0) {
            // Pad prominent parts to avoid false proximity
            text += prominentParts.join('\n' + ' '.repeat(50) + '\n') + '\n' + ' '.repeat(50) + '\n';
          }
        } catch (e) {
          // tagging failed, continue without the bonus
        }

        // Use Thunderbird's built-in converter for the full body plain text.
        // This handles encoded entities, table layouts, and nested elements
        // far better than a manual tag-strip.
        const plain = await messenger.messengerUtilities.convertToPlainText(part.content);
        text += plain + '\n';
      }
    }

    return text;
  } catch (e) {
    // strict_min_version guarantees both APIs exist, so this is a message
    // they choked on. Retry by walking the MIME tree ourselves.
    console.log("listInlineTextParts failed, falling back to getFull():", e.message);
    const full = await messenger.messages.getFull(messageId);
    return extractTextFromParts(full.parts);
  }
}

export async function processMessage(message) {
  console.log("Processing message");

  const bodyText = await getEmailText(message.id);

  // Subject is a strong prior — if it matches, we trust body extractions more
  const subjectMatches = OTP_SUBJECT_PATTERNS.some(p => p.test(message.subject));

  const candidates = [];

  for (const pattern of OTP_PATTERNS) {
    const regex = new RegExp(pattern.source, pattern.flags);
    let match;

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
    candidates.sort((a, b) => b.score - a.score);
    const best = candidates[0];

    if (best.score > 0) {
      console.log("Found OTP candidate in email with score:", best.score);
      const senderDomain = await getSenderDomain(message);
      sendToNativeHost({
        command: "new_otp",
        otp: best.num.replace(/[-\s]/g, ''),
        source: message.subject,
        sender_domain: senderDomain
      });
    }
  }
}

// One worker handles event pages, body reads and folder fallback queries.
// Claim IDs before awaiting so overlapping event sources cannot extract twice.
export function createMailMonitor(api, process, now = Date.now) {
  const seen = new Map();
  let work = Promise.resolve();
  const pendingFolders = new Map();
  let folderTimer;
  let recoveryTimer;
  let recoveryStopped = true;
  let recoveryGeneration = 0;
  let recoveryFolders = [];
  let recoveryIndex = 0;

  function enqueue(task) {
    work = work.then(task).catch(error => console.error('Tether mail monitor:', error));
    return work;
  }

  async function consume(page, displayed = false) {
    const cutoff = now() - OTP_TTL_MS;
    for (const [id, timestamp] of seen) {
      if (timestamp < cutoff) seen.delete(id);
    }
    while (page && Array.isArray(page.messages)) {
      for (const message of page.messages) {
        if (seen.has(message.id)) continue;
        // Opening old mail must not resurrect expired login codes.
        const date = new Date(message.date).getTime();
        if (!Number.isFinite(date) || date < now() - OTP_TTL_MS) continue;
        seen.set(message.id, now());
        const candidate = displayed || OTP_SUBJECT_PATTERNS.some(p => p.test(message.subject || '')) ||
          /noreply|no-reply|security|verify|auth|account/i.test(message.author || '');
        if (candidate) {
          try { await process(message); }
          catch (error) {
            seen.delete(message.id);
            console.error('Tether mail extraction:', error);
          }
        }
      }
      page = page.id ? await api.messages.continueList(page.id) : null;
    }
  }

  function received(folder, page) {
    return enqueue(() => consume(page));
  }

  function displayed(tabId, message) {
    return enqueue(() => consume({ messages: [message] }, true));
  }

  function folderChanged(folder, info) {
    // Unread/total counts also change on ordinary UI actions. Only new mail
    // warrants a fallback, and it must never turn into an all-account search.
    if (!(info.newMessageCount > 0) || !folder.id) return;
    pendingFolders.set(folder.id, folder);
    if (folderTimer) return;
    folderTimer = setTimeout(() => {
      folderTimer = undefined;
      const ids = [...pendingFolders.keys()];
      pendingFolders.clear();
      enqueue(async () => {
        for (const folderId of ids) {
          await consume(await api.recentMail.queryRecent(folderId, now() - OTP_TTL_MS));
        }
      });
    }, 1000);
  }

  // Date filters do not avoid Thunderbird's header enumeration. Spread recovery
  // across folders instead of starting an all-account query every 15 seconds.
  // Do not gate on counts: replacement/moves can leave folder counts unchanged.
  function startRecovery() {
    if (!recoveryStopped) return;
    recoveryStopped = false;
    const generation = ++recoveryGeneration;
    const schedule = delay => {
      if (!recoveryStopped && generation === recoveryGeneration) recoveryTimer = setTimeout(tick, delay);
    };
    async function tick() {
      recoveryTimer = undefined;
      const started = now();
      await enqueue(async () => {
        if (recoveryStopped || generation !== recoveryGeneration) return;
        if (recoveryIndex >= recoveryFolders.length) {
          // Folder discovery excludes virtual views (duplicate messages) and
          // empty folders without opening every folder's message database.
          recoveryFolders = await api.folders.query({
            isRoot: false, isVirtual: false, isUnified: false,
            hasMessages: true
          });
          recoveryIndex = 0;
        }
        const folder = recoveryFolders[recoveryIndex++];
        if (folder && !recoveryStopped) {
          await consume(await api.recentMail.queryRecent(folder.id, now() - OTP_TTL_MS));
        }
      });
      // At least one second idle after each folder, and target a minute per
      // sweep for small profiles. No interval can queue overlapping sweeps.
      schedule(Math.max(1000, 60000 / Math.max(1, recoveryFolders.length) - (now() - started)));
    }
    schedule(30000);
  }

  function stop() {
    recoveryStopped = true;
    recoveryGeneration++;
    clearTimeout(recoveryTimer);
    clearTimeout(folderTimer);
    pendingFolders.clear();
  }

  return { received, displayed, folderChanged, startRecovery, stop, idle: () => work };
}

if (typeof messenger !== 'undefined') {
  const monitor = createMailMonitor(messenger, processMessage);
  // Thunderbird delivers this event for IMAP too, after filters and junk
  // classification. Monitor all folders for OTPs moved by server-side rules.
  messenger.messages.onNewMailReceived.addListener(monitor.received, true);
  messenger.folders?.onFolderInfoChanged?.addListener(monitor.folderChanged);
  messenger.messageDisplay.onMessageDisplayed.addListener(monitor.displayed);
  monitor.startRecovery();
}
