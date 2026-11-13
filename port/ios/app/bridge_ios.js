// The iOS app's half of the launcher's bridge (port/launcher/BRIDGE.md, "Transport on a phone";
// finding 227). WKWebView has no way for the app to post a MessagePort to the page, so the app
// injects this script (port/ios/app/launcher.m, a WKUserScript at the document's end, in a content
// world of its own: the page's scripts can't see `dhdDeliver` or the message handler). It makes the
// channel the page's bridge.js waits for and posts one end to the page's window with the message
// "dhd-bridge", exactly as Android does; the other end carries the page's {id, call, args} text to
// the app (window.webkit.messageHandlers.dhd) and the app's answers and events back (dhdDeliver,
// which the app calls in this world). Nothing else is here.
'use strict';
var dhdDeliver = (function connect(window, native) {
  if (!native) return null;
  const channel = new MessageChannel();
  channel.port1.onmessage = (e) => native.postMessage(typeof e.data === 'string' ? e.data : JSON.stringify(e.data));
  window.postMessage('dhd-bridge', '*', [channel.port2]);
  return (text) => channel.port1.postMessage(text);
})(window, window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.dhd);
