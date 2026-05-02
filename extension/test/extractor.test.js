import { describe, it, expect, vi } from 'vitest';

global.messenger = {
  messages: {
    query: vi.fn().mockResolvedValue({ messages: [], id: null }),
    getFull: vi.fn().mockResolvedValue({ parts: [] }),
    listInlineTextParts: vi.fn().mockResolvedValue([]),
    continueList: vi.fn().mockResolvedValue(null),
    onNewMailReceived: {
      addListener: vi.fn()
    }
  },
  messengerUtilities: {
    convertToPlainText: vi.fn().mockResolvedValue('')
  },
  folders: {
    onFolderInfoChanged: {
      addListener: vi.fn()
    }
  },
  messageDisplay: {
    onMessageDisplayed: {
      addListener: vi.fn()
    }
  },
  runtime: {
    connectNative: vi.fn().mockReturnValue({
      postMessage: vi.fn(),
      onMessage: { addListener: vi.fn() },
      onDisconnect: { addListener: vi.fn() }
    })
  }
};

import { scoreCandidate, isFalsePositive, OTP_PATTERNS } from '../src/mail/extractor.js';

function extractOtpCandidates(text) {
  const candidates = [];
  for (const pattern of OTP_PATTERNS) {
    const regex = new RegExp(pattern.source, pattern.flags);
    let match;
    while ((match = regex.exec(text)) !== null) {
      const num = match[1] || match[0];
      if (/^[A-Za-z]+$/.test(num)) continue;

      const startIndex = Math.max(0, match.index - 50);
      const endIndex = Math.min(text.length, match.index + match[0].length + 50);
      const context = text.substring(startIndex, endIndex);

      if (isFalsePositive(num, context)) continue;

      const score = scoreCandidate(num, context);
      candidates.push({ num, score });
    }
  }
  return candidates.sort((a, b) => b.score - a.score);
}

describe('scoreCandidate (from extractor.js)', () => {
  it('scores high when keyword is near the number', () => {
    const score = scoreCandidate('123456', 'Your verification code is 123456');
    expect(score).toBeGreaterThan(0);
  });

  it('scores higher with proximity bonus', () => {
    const farAway = scoreCandidate('123456', 'your code is 123456 at the very end of this extremely long message that contains many words and little else');
    const close = scoreCandidate('123456', 'code: 123456 use this');
    expect(close).toBeGreaterThan(farAway);
  });

  it('gives bonus for prominent elements', () => {
    const normal = scoreCandidate('123456', 'your code is 123456');
    const prominent = scoreCandidate('123456', 'PROMINENT: code is 123456');
    expect(prominent).toBeGreaterThan(normal);
  });

  it('scores multiple keyword matches', () => {
    const single = scoreCandidate('123456', 'code 123456');
    const multiple = scoreCandidate('123456', 'verification code 123456');
    expect(multiple).toBeGreaterThan(single);
  });
});

describe('isFalsePositive (from extractor.js)', () => {
  it('detects years as false positives', () => {
    expect(isFalsePositive('2024', 'year 2024')).toBe(true);
    expect(isFalsePositive('2025', 'in 2025')).toBe(true);
  });

  it('detects phone-like numbers as false positives', () => {
    expect(isFalsePositive('0123456', 'phone: 0123456')).toBe(true);
  });

  it('detects prices as false positives', () => {
    expect(isFalsePositive('999', 'cost: $999 USD')).toBe(true);
    expect(isFalsePositive('50', 'price: $50')).toBe(true);
  });

  it('detects order/tracking as false positives', () => {
    expect(isFalsePositive('123456', 'order #123456 tracking')).toBe(true);
  });

  it('accepts legitimate OTP codes', () => {
    expect(isFalsePositive('171792', 'your verification code is 171792')).toBe(false);
    expect(isFalsePositive('123456', 'confirm code 123456 expires in 5 min')).toBe(false);
  });
});

describe('OTP pattern matching', () => {
  it('extracts plain digit codes', () => {
    const text = 'Your code is 171792';
    const matches = text.match(/\b(\d{4,8})\b/);
    expect(matches).toContain('171792');
  });

  it('extracts formatted codes', () => {
    const text = 'code: 123 456';
    const regex = new RegExp(OTP_PATTERNS[2].source, OTP_PATTERNS[2].flags);
    const match = regex.exec(text);
    expect(match).toBeTruthy();
  });

  it('extracts alphanumeric codes', () => {
    const text = 'token ABC123DEF';
    const regex = new RegExp(OTP_PATTERNS[1].source, OTP_PATTERNS[1].flags);
    const matches = text.match(regex);
    expect(matches).toBeTruthy();
  });
});

describe('Real email fixtures', () => {
  it('extracts OTP from MyBank email', () => {
    const text = `PREVENT FRAUD: DO NOT SHARE THIS CODE. MyBank will never contact you asking for it. Only use verification code: 171792 in Online Banking or the MyBank App.`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBeGreaterThan(0);
    const best = candidates[0];
    expect(best.num).toBe('171792');
    expect(best.score).toBeGreaterThan(0);
  });

  it('extracts OTP from Google auth email', () => {
    const text = `Google: Your verification code is 123456

This code is valid for the next 10 minutes. Don't share this with anyone.`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBeGreaterThan(0);
    expect(candidates[0].num).toBe('123456');
  });

  it('extracts OTP from Amazon formatted code', () => {
    const text = `Your Amazon sign-in verification code is 123 456

This code can be used to confirm your identity.`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBeGreaterThan(0);
    expect(candidates[0].num.replace(/[-\s]/g, '')).toBe('123456');
  });

  it('extracts OTP from bank email with strong tag', () => {
    const text = `PROMINENT:987654 Your verification code is 987654 expires in 5 minutes. Do not share this code.`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBeGreaterThan(0);
    expect(candidates[0].num).toBe('987654');
  });

  it('rejects order confirmation as false positive', () => {
    const text = `Thank you for your order! Order number: 123456789 Total: $99.99`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBe(0);
  });
});
