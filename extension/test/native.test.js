import { describe, it, expect, vi, beforeEach } from 'vitest';

// Capture the onMessage listener registered by connectToNativeHost so we can
// simulate the daemon pushing events to the native host.
let messageListener = null;

const mockPort = {
  postMessage: vi.fn(),
  onMessage: { addListener: vi.fn((cb) => { messageListener = cb; }) },
  onDisconnect: { addListener: vi.fn() }
};

beforeEach(() => {
  messageListener = null;
  mockPort.postMessage.mockClear();

  global.chrome = {
    runtime: {
      connectNative: vi.fn().mockReturnValue(mockPort),
      lastError: null
    },
    tabs: {
      // native.js uses the callback form of query()
      query: vi.fn((_filter, cb) => cb([{ id: 1 }, { id: 2 }, { id: 3 }])),
      sendMessage: vi.fn((_id, _msg, cb) => { if (cb) cb(); })
    }
  };
  // Force native.js down the chrome.* branch.
  global.browser = undefined;
});

import { connectToNativeHost } from '../src/shared/native.js';

describe('native host receive/routing', () => {
  it('delivers otp_available to every tab, not just the active one', () => {
    connectToNativeHost();
    expect(messageListener).toBeTypeOf('function');

    messageListener({ command: 'otp_available', otp: '123456' });

    expect(chrome.tabs.query).toHaveBeenCalledWith({}, expect.any(Function));
    expect(chrome.tabs.sendMessage).toHaveBeenCalledTimes(3);
    for (const id of [1, 2, 3]) {
      expect(chrome.tabs.sendMessage).toHaveBeenCalledWith(
        id,
        { action: 'fill_otp', otp: '123456' },
        expect.any(Function)
      );
    }
  });

  it('ignores an otp_available event with an empty code', () => {
    connectToNativeHost();
    expect(messageListener).toBeTypeOf('function');

    messageListener({ command: 'otp_available', otp: '' });

    expect(chrome.tabs.sendMessage).not.toHaveBeenCalled();
  });
});
