import { describe, it, expect, vi, afterEach } from 'vitest';
import { createMailMonitor } from '../src/mail/extractor.js';

const now = 1700000000000;
const message = id => ({ id, date: new Date(now), subject: 'Your login code', author: 'security@example.com' });
function setup(process = vi.fn().mockResolvedValue()) {
  const api = { messages: {
    query: vi.fn().mockResolvedValue({ messages: [] }),
    continueList: vi.fn().mockResolvedValue({ messages: [message(2)] })
  } };
  api.recentMail = { queryRecent: vi.fn((folderId, fromTime) => api.messages.query({folderId, includeSubFolders:false, fromDate:new Date(fromTime)})) };
  return { api, process, monitor: createMailMonitor(api, process, () => now) };
}
afterEach(() => vi.useRealTimers());
describe('mail event processing', () => {
  it('does no startup or recurring queries when idle', async () => {
    vi.useFakeTimers();
    const { api, monitor } = setup();
    await vi.advanceTimersByTimeAsync(120000);
    await monitor.idle();
    expect(api.messages.query).not.toHaveBeenCalled();
  });
  it('walks event pages and deduplicates display/event overlap', async () => {
    const { api, process, monitor } = setup();
    monitor.received({}, { messages: [message(1)], id: 'next' });
    await monitor.displayed(1, message(2));
    expect(api.messages.continueList).toHaveBeenCalledWith('next');
    expect(process.mock.calls.map(([m]) => m.id)).toEqual([1, 2]);
    expect(api.messages.query).not.toHaveBeenCalled();
  });
  it('ignores ordinary folder changes and debounces new mail to its folder', async () => {
    vi.useFakeTimers();
    const { api, monitor } = setup();
    monitor.folderChanged({ id: 'inbox' }, { unreadMessageCount: 12 });
    monitor.folderChanged({}, { newMessageCount: 1 });
    expect(vi.getTimerCount()).toBe(0);
    monitor.folderChanged({ id: 'inbox' }, { newMessageCount: 1 });
    monitor.folderChanged({ id: 'inbox' }, { newMessageCount: 2 });
    await vi.advanceTimersByTimeAsync(1000);
    await monitor.idle();
    expect(api.messages.query).toHaveBeenCalledExactlyOnceWith({ folderId: 'inbox', includeSubFolders: false, fromDate: new Date(now - 600000) });
  });
  it('serializes body reads even across concurrent events', async () => {
    let release;
    const process = vi.fn().mockImplementationOnce(() => new Promise(r => { release = r; })).mockResolvedValue();
    const { monitor } = setup(process);
    monitor.received({}, { messages: [message(1)] });
    const done = monitor.received({}, { messages: [message(2)] });
    await Promise.resolve();
    expect(process).toHaveBeenCalledTimes(1);
    release();
    await done;
    expect(process).toHaveBeenCalledTimes(2);
  });
  it('rechecks expiry after a slow paginated read', async () => {
    let clock = now;
    const process = vi.fn().mockImplementation(async () => { clock += 1000; });
    const api = { messages: { continueList: vi.fn().mockResolvedValue({
      messages: [{...message(2), date: new Date(now - 599500)}]
    }) } };
    const monitor = createMailMonitor(api, process, () => clock);
    await monitor.received({}, {messages:[message(1)],id:'next'});
    expect(process.mock.calls.map(([m])=>m.id)).toEqual([1]);
  });
  it('rejects expired messages including displayed mail', async () => {
    const { process, monitor } = setup();
    await monitor.displayed(1, { ...message(1), date: new Date(now - 600001) });
    expect(process).not.toHaveBeenCalled();
  });
});


describe('paced missed-event recovery', () => {
  function recoverySetup(process = vi.fn().mockResolvedValue()) {
    const state = setup(process);
    state.api.folders = { query: vi.fn().mockResolvedValue([{ id: 'inbox' }, { id: 'filtered' }]) };
    return state;
  }
  it('catches existing recent mail and count-neutral arrivals without either event', async () => {
    vi.useFakeTimers();
    const { api, process, monitor } = recoverySetup();
    api.messages.query.mockResolvedValueOnce({ messages: [message(1)] });
    monitor.startRecovery();
    await vi.advanceTimersByTimeAsync(29999);
    expect(api.messages.query).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1);
    expect(process).toHaveBeenCalledWith(message(1));
    expect(api.messages.query).toHaveBeenCalledWith({folderId:'inbox',includeSubFolders:false,fromDate:new Date(now-600000)});
    await vi.advanceTimersByTimeAsync(30000);
    expect(api.messages.query).toHaveBeenLastCalledWith({folderId:'filtered',includeSubFolders:false,fromDate:new Date(now-600000)});
    api.messages.query.mockResolvedValueOnce({ messages: [message(2)] });
    await vi.advanceTimersByTimeAsync(30000);
    expect(process.mock.calls.map(([m])=>m.id)).toEqual([1,2]);
    expect(api.folders.query).toHaveBeenCalledTimes(2);
    monitor.stop();
  });
  it('does not queue repeated recovery while a query is outstanding', async () => {
    vi.useFakeTimers();
    const { api, monitor } = recoverySetup();
    let release;
    api.messages.query.mockImplementationOnce(()=>new Promise(r=>{release=r;}));
    monitor.startRecovery();monitor.startRecovery();
    await vi.advanceTimersByTimeAsync(30000);
    await vi.advanceTimersByTimeAsync(180000);
    expect(api.messages.query).toHaveBeenCalledTimes(1);
    release({messages:[]});await monitor.idle();
    monitor.stop();
    await vi.advanceTimersByTimeAsync(180000);
    expect(api.messages.query).toHaveBeenCalledTimes(1);
  });
  it('does not revive an old timer when recovery is restarted mid-query', async () => {
    vi.useFakeTimers();
    const {api, monitor} = recoverySetup();
    let release;
    api.messages.query.mockImplementationOnce(()=>new Promise(r=>{release=r;}));
    monitor.startRecovery();
    await vi.advanceTimersByTimeAsync(30000);
    monitor.stop();monitor.startRecovery();
    release({messages:[]});await monitor.idle();
    await vi.advanceTimersByTimeAsync(30000);
    expect(api.messages.query).toHaveBeenCalledTimes(2);
    expect(vi.getTimerCount()).toBe(1);
    monitor.stop();
  });
  it('keeps events deduplicated and rejects expired mail during recovery', async () => {
    vi.useFakeTimers();
    const { api, process, monitor } = recoverySetup();
    await monitor.received({}, {messages:[message(1)]});
    api.messages.query.mockResolvedValueOnce({messages:[message(1),{...message(3),date:new Date(now-600001)},message(2)]});
    monitor.startRecovery();
    await vi.advanceTimersByTimeAsync(30000);
    expect(process.mock.calls.map(([m])=>m.id)).toEqual([1,2]);
    monitor.stop();
  });
  it('uses a fresh recovery window after a long suspend', async () => {
    vi.useFakeTimers();
    const state = recoverySetup();
    let clock = now;
    const monitor = createMailMonitor(state.api,state.process,()=>clock);
    monitor.startRecovery();
    clock += 24*60*60*1000;
    const fresh = {...message(2),date:new Date(clock)};
    state.api.messages.query.mockResolvedValueOnce({messages:[message(1),fresh]});
    await vi.advanceTimersByTimeAsync(30000);
    expect(state.api.recentMail.queryRecent).toHaveBeenCalledWith('inbox',clock-600000);
    expect(state.process).toHaveBeenCalledExactlyOnceWith(fresh);
    monitor.stop();
  });
  it('retries after failed discovery and includes newly created folders next sweep', async () => {
    vi.useFakeTimers();
    const { api, monitor } = recoverySetup();
    api.folders.query.mockRejectedValueOnce(new Error('offline'));
    monitor.startRecovery();
    await vi.advanceTimersByTimeAsync(30000);
    expect(api.messages.query).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(60000);
    expect(api.messages.query).toHaveBeenCalledTimes(1);
    monitor.stop();
  });
});
