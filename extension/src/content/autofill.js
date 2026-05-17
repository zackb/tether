// Content script for browsers (Chrome/Firefox)
// This runs in the context of webpages and looks for OTP input fields

export function findOtpInputs(doc) {
  const inputs = [...doc.querySelectorAll('input')];

  return inputs.filter(input => {
    const attrs = [input.id, input.name, input.placeholder, input.className, input.type]
      .join(' ').toLowerCase();

    // Reject obvious non-OTP fields immediately
    if (/\b(zip|search|password|email|phone)\b/.test(attrs)) return false;

    // Tier 1: explicit standard attribute
    if (input.autocomplete === 'one-time-code') return true;

    // Tier 2: type/inputmode signals
    if (input.inputMode === 'numeric') return scoreInput(input) > 5;
    if (input.type === 'tel' || input.type === 'number') return scoreInput(input) > 5;

    // Tier 3: attribute keyword matching
    if (/\b(otp|passcode|verif|token|pin|2fa)\b/.test(attrs)) return true;

    // Tier 4: maxlength in OTP range (4–8)
    const ml = parseInt(input.maxLength);
    if (ml >= 4 && ml <= 8) return scoreInput(input) > 3;

    // Tier 5: fallback for generic text inputs based on label/context
    if (input.type === 'text' || !input.type) {
      if (scoreInput(input) >= 10) return true;
    }

    return false;
  });
}

export function scoreInput(input) {
  let score = 0;
  let labelText = input.labels?.[0]?.textContent || '';
  if (!labelText && input.id) {
    try {
      const doc = input.ownerDocument || document;
      const labelEl = doc.querySelector(`label[for="${input.id}"]`);
      if (labelEl) labelText = labelEl.textContent;
    } catch (e) {}
  }
  if (!labelText) {
    labelText = input.closest('form')?.querySelector('label')?.textContent || '';
  }

  if (/code|otp|verify|token|pin/i.test(labelText)) score += 10;

  // Surrounding text in parent
  const parentText = input.parentElement?.textContent?.toLowerCase() || '';
  if (/enter.*code|verification|one.time/i.test(parentText)) score += 5;

  return score;
}

export function detectSplitOtp(doc) {
  const inputs = [...doc.querySelectorAll('input')]
    .filter(i => i.type === 'text' || i.type === 'tel' || i.type === 'number' || !i.type);

  // Strategy 1: exact maxlength=1 (standard split OTP)
  const singles = inputs.filter(i => i.maxLength === 1);
  if (singles.length >= 4 && singles.length <= 8) {
    return singles;
  }

  // Strategy 2: Clustered inputs under same parent with OTP keywords (e.g. Walmart)
  const parentMap = new Map();
  inputs.forEach(input => {
    const p = input.parentElement;
    if (p) {
      if (!parentMap.has(p)) parentMap.set(p, []);
      parentMap.get(p).push(input);
    }
  });

  for (const group of parentMap.values()) {
    if (group.length >= 4 && group.length <= 8) {
      const otpScore = group.reduce((acc, input) => {
        const attrs = [input.id, input.name, input.className, input.getAttribute('aria-label')]
          .join(' ').toLowerCase();
        if (/\botp\b|\bcode\b|verif|digit/.test(attrs)) return acc + 1;
        return acc;
      }, 0);

      // If at least half the inputs in the group look like OTP inputs
      if (otpScore >= group.length / 2) return group;
    }
  }

  return null;
}

// ---------------------------------------------------------------------------
// Browser-only code - only runs when loaded in actual browser context
// ---------------------------------------------------------------------------
if (typeof document === 'undefined') {
  // Test environment: skip browser-only code
} else {
let otpInterval = null;

function requestOtp() {
  chrome.runtime.sendMessage({ 
    action: "request_otp_for_site", 
    url: window.location.hostname 
  });
}

function hasOtpFields() {
  if (detectSplitOtp(document)) return true;
  return findOtpInputs(document).length > 0;
}

// Check every 2 seconds for up to 2 minutes
if (hasOtpFields()) {
  console.log("Found potential OTP fields on page, requesting OTP from Tether");
  requestOtp();

  let attempts = 0;
  otpInterval = setInterval(() => {
    attempts++;
    const splits = detectSplitOtp(document);
    const regular = findOtpInputs(document);

    // Stop trying after 2 minutes or if the user manually filled the field
    if (attempts > 60 || 
        (splits && splits[0].value !== "") || 
        (regular.length > 0 && regular[0].value.length > 3)) {
      clearInterval(otpInterval);
      return;
    }
    requestOtp();
  }, 2000);
}

// Listen for OTPs sent from the daemon
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === "fill_otp" && request.otp) {
    const otp = request.otp.toString();
    const splitFields = detectSplitOtp(document);

    if (splitFields) {
      console.log("Autofilling split OTP:", otp);
      for (let i = 0; i < Math.min(otp.length, splitFields.length); i++) {
        splitFields[i].value = otp[i];

        // Dispatch events so React/Vue/Angular pick up the change
        splitFields[i].dispatchEvent(new Event('input', { bubbles: true }));
        splitFields[i].dispatchEvent(new Event('change', { bubbles: true }));
      }
      if (otpInterval) clearInterval(otpInterval);
    } else {
      const regularFields = findOtpInputs(document);
      if (regularFields.length > 0 && regularFields[0].value === "") {
        console.log("Autofilling OTP:", otp);
        regularFields[0].value = otp;

        // Dispatch events so React/Vue/Angular pick up the change
        regularFields[0].dispatchEvent(new Event('input', { bubbles: true }));
        regularFields[0].dispatchEvent(new Event('change', { bubbles: true }));

        // Clean up the interval
        if (otpInterval) clearInterval(otpInterval);
      }
    }
  }
});
}
