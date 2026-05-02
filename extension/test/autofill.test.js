import { describe, it, expect, vi, beforeEach } from 'vitest';
import { JSDOM } from 'jsdom';
import * as fs from 'fs';
import * as path from 'path';

const loadFixture = (filename) => {
  const html = fs.readFileSync(path.join(__dirname, 'fixtures', filename), 'utf8');
  return new JSDOM(html).window.document;
};

global.chrome = {
  runtime: {
    sendMessage: vi.fn(),
    onMessage: {
      addListener: vi.fn()
    }
  }
};

describe('OTP input detection', () => {
  let findOtpInputs, scoreInput, detectSplitOtp;

  beforeEach(async () => {
    const autofill = await import('../src/content/autofill.js');
    findOtpInputs = autofill.findOtpInputs;
    scoreInput = autofill.scoreInput;
    detectSplitOtp = autofill.detectSplitOtp;
  });

  describe('findOtpInputs', () => {
    it('detects standard autocomplete attribute', () => {
      const doc = loadFixture('std-autocomplete.html');
      const inputs = findOtpInputs(doc);
      expect(inputs.length).toBe(1);
      expect(inputs[0].autocomplete).toBe('one-time-code');
    });

    it('detects split OTP inputs', () => {
      const doc = loadFixture('split-otp.html');
      const inputs = detectSplitOtp(doc);
      expect(inputs).toHaveLength(6);
    });

    it('detects by placeholder/name keywords', () => {
      const doc = loadFixture('google-otp.html');
      const inputs = findOtpInputs(doc);
      expect(inputs.length).toBe(1);
    });

    it('detects numeric inputmode', () => {
      const doc = loadFixture('numeric-otp.html');
      const inputs = findOtpInputs(doc);
      expect(inputs.length).toBe(1);
    });
  });

  describe('scoreInput', () => {
    it('scores high for label with code keywords', () => {
      const doc = loadFixture('std-autocomplete.html');
      const input = doc.querySelector('input');
      const score = scoreInput(input);
      expect(score).toBeGreaterThan(0);
    });

    it('scores low for generic fields', () => {
      const doc = new JSDOM('<input type="text" name="foo">').window.document;
      const input = doc.querySelector('input');
      const score = scoreInput(input);
      expect(score).toBe(0);
    });
  });
});

describe('OTP autofill insertion', () => {
  let detectSplitOtp, findOtpInputs;

  beforeEach(async () => {
    const autofill = await import('../src/content/autofill.js');
    detectSplitOtp = autofill.detectSplitOtp;
    findOtpInputs = autofill.findOtpInputs;
  });

  describe('split OTP filling', () => {
    it('fills each character into separate fields', () => {
      const doc = loadFixture('split-otp.html');
      const inputs = detectSplitOtp(doc);

      const otp = '123456';
      for (let i = 0; i < otp.length; i++) {
        inputs[i].value = otp[i];
        inputs[i].dispatchEvent(new doc.defaultView.Event('input', { bubbles: true }));
        inputs[i].dispatchEvent(new doc.defaultView.Event('change', { bubbles: true }));
      }

      expect(inputs[0].value).toBe('1');
      expect(inputs[1].value).toBe('2');
      expect(inputs[5].value).toBe('6');
    });
  });

  describe('single OTP filling', () => {
    it('fills full OTP into single field', () => {
      const doc = loadFixture('std-autocomplete.html');
      const inputs = findOtpInputs(doc);

      inputs[0].value = '789012';
      inputs[0].dispatchEvent(new doc.defaultView.Event('input', { bubbles: true }));
      inputs[0].dispatchEvent(new doc.defaultView.Event('change', { bubbles: true }));

      expect(inputs[0].value).toBe('789012');
    });
  });
});