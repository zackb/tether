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

// Request OTP from daemon when we detect OTP fields
const fields = findOtpFields();
if (fields.length > 0) {
  console.log("Found potential OTP fields on page, requesting OTP from Tether");
  
  chrome.runtime.sendMessage({ 
    action: "request_otp_for_site", 
    url: window.location.hostname 
  });
}

// Listen for OTPs sent from the daemon
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === "fill_otp" && request.otp) {
    const fields = findOtpFields();
    if (fields.length > 0) {
      console.log("Autofilling OTP:", request.otp);
      // Basic autofill logic
      fields[0].value = request.otp;
      
      // Dispatch events so React/Vue/Angular pick up the change
      fields[0].dispatchEvent(new Event('input', { bubbles: true }));
      fields[0].dispatchEvent(new Event('change', { bubbles: true }));
    }
  }
});
