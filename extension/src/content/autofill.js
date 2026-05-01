// Content script for browsers (Chrome/Firefox)
// This runs in the context of webpages and looks for OTP input fields

function findOtpFields() {
  const inputs = document.querySelectorAll('input[type="text"], input[type="number"], input[autocomplete="one-time-code"]');
  const otpFields = [];
  
  for (const input of inputs) {
    const name = (input.name || '').toLowerCase();
    const id = (input.id || '').toLowerCase();
    
    if (name.includes('otp') || id.includes('otp') || name.includes('code') || input.autocomplete === 'one-time-code') {
      otpFields.push(input);
    }
  }
  
  return otpFields;
}

let otpInterval = null;

function requestOtp() {
  chrome.runtime.sendMessage({ 
    action: "request_otp_for_site", 
    url: window.location.hostname 
  });
}

// Check every 2 seconds for up to 2 minutes
const fields = findOtpFields();
if (fields.length > 0) {
  console.log("Found potential OTP fields on page, requesting OTP from Tether");
  requestOtp();

  let attempts = 0;
  otpInterval = setInterval(() => {
    attempts++;
    // Stop trying after 2 minutes or if the user manually filled the field
    if (attempts > 60 || (fields[0] && fields[0].value.length > 4)) {
      clearInterval(otpInterval);
      return;
    }
    requestOtp();
  }, 2000);
}

// Listen for OTPs sent from the daemon
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === "fill_otp" && request.otp) {
    const fields = findOtpFields();
    if (fields.length > 0 && fields[0].value === "") {
      console.log("Autofilling OTP:", request.otp);
      // Basic autofill logic
      fields[0].value = request.otp;

      // Dispatch events so React/Vue/Angular pick up the change
      fields[0].dispatchEvent(new Event('input', { bubbles: true }));
      fields[0].dispatchEvent(new Event('change', { bubbles: true }));

      // Clean up the interval
      if (otpInterval) clearInterval(otpInterval);
    }
  }
});
