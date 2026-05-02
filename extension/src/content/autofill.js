// Content script for browsers (Chrome/Firefox)
// This runs in the context of webpages and looks for OTP input fields

function findOtpInputs(doc) {
  const inputs = [...doc.querySelectorAll('input')];

  return inputs.filter(input => {
    // Tier 1: explicit standard attribute
    if (input.autocomplete === 'one-time-code') return true;

    // Tier 2: type/inputmode signals
    if (input.inputMode === 'numeric') return scoreInput(input) > 5;
    if (input.type === 'tel' || input.type === 'number') return scoreInput(input) > 5;

    // Tier 3: attribute keyword matching
    const attrs = [input.id, input.name, input.placeholder, input.className]
      .join(' ').toLowerCase();
    if (/otp|passcode|verif|code|token|pin|2fa/.test(attrs)) return true;

    // Tier 4: maxlength in OTP range (4–8)
    const ml = parseInt(input.maxLength);
    if (ml >= 4 && ml <= 8 && input.type !== 'password') return scoreInput(input) > 3;

    return false;
  });
}

function scoreInput(input) {
  let score = 0;
  // Look at nearby label text
  const label = input.labels?.[0]?.textContent
    || input.closest('form')?.querySelector('label')?.textContent || '';
  if (/code|otp|verify|token|pin/i.test(label)) score += 10;

  // Surrounding text in parent
  const parentText = input.parentElement?.textContent?.toLowerCase() || '';
  if (/enter.*code|verification|one.time/i.test(parentText)) score += 5;

  return score;
}

function detectSplitOtp(doc) {
  const singles = [...doc.querySelectorAll('input[maxlength="1"]')]
    .filter(i => i.type === 'text' || i.type === 'tel' || i.type === 'number' || !i.type);

  // Check if they're siblings/adjacent — likely a split OTP
  if (singles.length >= 4 && singles.length <= 8) {
    return singles; // fill character by character
  }
  return null;
}

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
