import { describe, it, expect, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

function setup(headers, { virtual = false, onYield } = {}) {
  let clock = 1700000000000;
  let close;
  const convert = vi.fn(header => ({ id: header.id, date: new Date(header.dateInSeconds * 1000) }));
  const yieldTask = vi.fn(callback => { onYield?.(() => close()); callback(); });
  const scope = {
    ChromeUtils: { importESModule: () => ({ setTimeout:yieldTask, getFolder: () => ({folder: {
      isServer: false, getFlag: () => virtual, messages: headers
    }}) }) },
    ExtensionCommon: { ExtensionAPI: class {} }, ExtensionUtils: {ExtensionError: Error},
    Ci: { nsMsgFolderFlags: {Virtual: 1} }, Date: {now: () => clock++},
    setTimeout: yieldTask
  };
  vm.runInNewContext(readFileSync(new URL('../experiments/recentMail/api.js', import.meta.url), 'utf8'), scope);
  const api = new scope.recentMail().getAPI({
    callOnClose: handler => { close = () => handler.close(); },
    extension: {messageManager: {convert}}
  }).recentMail;
  return {api, convert, yieldTask, now: clock};
}

describe('cooperative parent-thread mail search', () => {
  it('yields during a large search and converts only recent headers', async () => {
    const now = 1700000000000;
    const headers = Array.from({length: 10000}, (_, id) => ({id, dateInSeconds: (now - 3600000) / 1000}));
    headers.push({id:10000,dateInSeconds:now/1000});
    const {api,convert,yieldTask} = setup(headers);
    const page = await api.queryRecent('inbox', now - 600000);
    expect(page.messages.map(m => m.id)).toEqual([10000]);
    expect(convert).toHaveBeenCalledTimes(1);
    expect(yieldTask.mock.calls.length).toBeGreaterThan(100);
  });
  it('enforces the ten-minute limit even if a caller requests older mail', async () => {
    const now = 1700000000000;
    const {api,convert} = setup([{id:1,dateInSeconds:(now-600001)/1000}]);
    expect((await api.queryRecent('inbox', 0)).messages).toEqual([]);
    expect(convert).not.toHaveBeenCalled();
  });
  it('stops a running iterator when the extension closes', async () => {
    const now = 1700000000000;
    const {api} = setup(Array.from({length:100},(_,id)=>({id,dateInSeconds:now/1000})), {onYield:close=>close()});
    await expect(api.queryRecent('inbox', now-600000)).rejects.toThrow('stopped');
  });
  it('rejects virtual views to avoid scanning duplicate physical folders', async () => {
    const {api} = setup([], {virtual:true});
    await expect(api.queryRecent('virtual',0)).rejects.toThrow('physical');
  });
});
