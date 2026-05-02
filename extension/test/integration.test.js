import { describe, it, expect, vi, beforeEach } from 'vitest';
import { JSDOM } from 'jsdom';
import { simpleParser } from 'mailparser';
import fs from 'fs';
import path from 'path';

const testEmailsDir = path.join(process.cwd(), 'test', 'fixtures');

const dom = new JSDOM('');
global.DOMParser = dom.window.DOMParser;
global.window = dom.window;

let extractedOtp = null;

vi.mock('../src/shared/native.js', () => ({
  connectToNativeHost: vi.fn(),
  sendToNativeHost: vi.fn((msg) => {
    if (msg.command === 'new_otp') {
      extractedOtp = msg.otp;
    }
  })
}));

const mockPort = {
  postMessage: vi.fn(),
  onMessage: { addListener: vi.fn() },
  onDisconnect: { addListener: vi.fn() }
};

global.browser = {
  runtime: {
    connectNative: vi.fn().mockReturnValue(mockPort)
  },
  tabs: {
    query: vi.fn().mockResolvedValue([]),
    sendMessage: vi.fn()
  }
};

global.chrome = {
  runtime: {
    connectNative: vi.fn().mockReturnValue(mockPort),
    lastError: null
  },
  tabs: {
    query: vi.fn().mockResolvedValue([]),
    sendMessage: vi.fn()
  }
};

global.messenger = {
  messages: {
    query: vi.fn().mockResolvedValue({ messages: [], id: null }),
    getFull: vi.fn(),
    listInlineTextParts: vi.fn(),
    continueList: vi.fn().mockResolvedValue(null),
    onNewMailReceived: { addListener: vi.fn() }
  },
  messengerUtilities: {
    convertToPlainText: vi.fn().mockResolvedValue('')
  },
  folders: { onFolderInfoChanged: { addListener: vi.fn() } },
  messageDisplay: { onMessageDisplayed: { addListener: vi.fn() } },
  runtime: {
    connectNative: vi.fn().mockReturnValue(mockPort)
  }
};

async function loadEmlAsParts(filePath) {
  const buffer = fs.readFileSync(filePath);
  const parsed = await simpleParser(buffer);

  const parts = [];

  if (parsed.text) {
    parts.push({ contentType: 'text/plain', content: parsed.text });
  }

  if (parsed.html) {
    parts.push({ contentType: 'text/html', content: parsed.html });
  }

  return parts;
}

import { processMessage } from '../src/mail/extractor.js';

describe('Integration tests with real .eml files', () => {
  beforeEach(() => {
    extractedOtp = null;
  });

  it('processes MyBank OTP email and extracts 171792', async () => {
    const emailPath = path.join(testEmailsDir, 'mybank-otp.eml');
    const parts = await loadEmlAsParts(emailPath);

    messenger.messages.listInlineTextParts.mockResolvedValue(parts);

    const message = {
      id: 1,
      subject: 'MyBank One Time Password',
      author: 'memberservice@mybank.com'
    };

    await processMessage(message);

    expect(extractedOtp).toBe('171792');
  });

  it('processes MyBank OTP email with HTML and extracts 171793', async () => {
    const emailPath = path.join(testEmailsDir, 'mybank-html-otp.eml');
    const parts = await loadEmlAsParts(emailPath);

    messenger.messages.listInlineTextParts.mockResolvedValue(parts);

    const message = {
      id: 1,
      subject: 'MyBank One Time Password',
      author: 'memberservice@mybank.com'
    };

    await processMessage(message);

    expect(extractedOtp).toBe('171793');
  });

  it('processes Google auth email and extracts 123456', async () => {
    const emailPath = path.join(testEmailsDir, 'google-auth.eml');
    const parts = await loadEmlAsParts(emailPath);

    messenger.messages.listInlineTextParts.mockResolvedValue(parts);

    const message = {
      id: 2,
      subject: 'Your Google verification code',
      author: 'noreply@google.com'
    };

    await processMessage(message);

    expect(extractedOtp).toBe('123456');
  });

  it('processes Amazon formatted code and extracts 123456', async () => {
    const emailPath = path.join(testEmailsDir, 'amazon-format.eml');
    const parts = await loadEmlAsParts(emailPath);

    messenger.messages.listInlineTextParts.mockResolvedValue(parts);

    const message = {
      id: 3,
      subject: 'Amazon: Sign-in verification code',
      author: 'ship-confirm@amazon.com'
    };

    await processMessage(message);

    expect(extractedOtp.replace(/[-\s]/g, '')).toBe('123456');
  });

  it('processes bank verification email and extracts 987654', async () => {
    const emailPath = path.join(testEmailsDir, 'bank-verification.eml');
    const parts = await loadEmlAsParts(emailPath);

    messenger.messages.listInlineTextParts.mockResolvedValue(parts);

    const message = {
      id: 4,
      subject: 'Verify your identity',
      author: 'noreply@chase.com'
    };

    await processMessage(message);

    expect(extractedOtp).toBe('987654');
  });

  it('rejects order confirmation (no OTP extraction)', async () => {
    const emailPath = path.join(testEmailsDir, 'order-confirmation.eml');
    const parts = await loadEmlAsParts(emailPath);

    messenger.messages.listInlineTextParts.mockResolvedValue(parts);

    const message = {
      id: 5,
      subject: 'Your Amazon order confirmation',
      author: 'order-update@amazon.com'
    };

    await processMessage(message);

    expect(extractedOtp).toBeNull();
  });
});
