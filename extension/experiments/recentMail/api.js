// Thunderbird's messages.query() first copies every header into a JS array.
// Iterate lazily and yield between small batches so large folders cannot turn
// recovery into an uninterrupted parent/UI-thread scan. No mail is modified.
var { getFolder } = ChromeUtils.importESModule(
  'resource:///modules/ExtensionAccounts.sys.mjs'
);
var { setTimeout } = ChromeUtils.importESModule('resource://gre/modules/Timer.sys.mjs');
var { ExtensionError } = ExtensionUtils;
var recentMail = class extends ExtensionCommon.ExtensionAPI {
  getAPI(context) {
    let closed = false;
    context.callOnClose({ close() { closed = true; } });
    return {
      recentMail: {
        async queryRecent(folderId, fromTime) {
          const { folder } = getFolder(folderId);
          if (folder.isServer || folder.getFlag(Ci.nsMsgFolderFlags.Virtual)) {
            throw new ExtensionError('Recovery requires a physical mail folder');
          }
          // Never let a caller request an unrestricted or stale mailbox scan.
          const cutoff = Math.max(fromTime, Date.now() - 10 * 60 * 1000);
          const messages = [];
          let deadline = Date.now() + 5;
          for (const header of folder.messages) {
            if (closed) throw new ExtensionError('Mail extension stopped');
            if (header.dateInSeconds * 1000 >= cutoff) {
              messages.push(context.extension.messageManager.convert(header));
            }
            if (Date.now() >= deadline) {
              await new Promise(resolve => setTimeout(resolve, 0));
              deadline = Date.now() + 5;
            }
          }
          return { messages };
        }
      }
    };
  }
};
