// The page's safety net (finding 273), the first script and the only one in ES5: it parses and
// runs on any WebView, so it can say why the launcher did not start when the others can't.
//
// - A phone's layout from the start: html.touch on Android, iPhone and iPad (app.js sets it again
//   from the snapshot's caps), so a phone never shows the desktop's layout while the page loads.
// - Android System WebView older than MIN_CHROMIUM (the page's syntax and APIs need it; the test
//   test_launcher_ui.js keeps the page within it): a plain message with the version found and a
//   button to the WebView's Play Store page, instead of a page that does not react.
// - Any other start that fails (a script error, start() rejected) before app.js says it started: the
//   error in one line, so the player's report says what broke.
//
// Keep this file ES5: no let/const, arrow functions, template strings, classes or newer APIs.
'use strict';

var dhdBoot = (function () {
  var MIN_CHROMIUM = 80;
  var ua = (typeof navigator !== 'undefined' && navigator.userAgent) || '';
  var android = /Android/.test(ua);
  var match = /Chrome\/(\d+)/.exec(ua);
  var chromium = match ? parseInt(match[1], 10) : 0;
  var boot = { minChromium: MIN_CHROMIUM, chromium: chromium, android: android, started: false, stopped: false };
  if (typeof document === 'undefined') return boot;

  var phone = android || /iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1);
  if (phone) document.documentElement.className += ' touch';

  // The bridge's port (bridge.js takes the same one): the Play Store button's only way out.
  var port = null, queued = [];
  window.addEventListener('message', function (e) {
    if (port || e.data !== 'dhd-bridge' || !e.ports || !e.ports[0]) return;
    port = e.ports[0];
    for (var i = 0; i < queued.length; i++) port.postMessage(queued[i]);
    queued = [];
  });
  function openLink(url) {
    // id 0: bridge.js numbers its calls from 1 and ignores an answer it did not ask for
    var text = JSON.stringify({ id: 0, call: 'open_link', args: { url: url } });
    if (port) port.postMessage(text); else queued.push(text);
  }

  function show(title, text, button) {
    if (boot.stopped) return;
    boot.stopped = true;
    var draw = function () {
      var box = document.createElement('div');
      box.className = 'boot-fail';
      var h = document.createElement('h1');
      h.appendChild(document.createTextNode(title));
      var p = document.createElement('p');
      p.appendChild(document.createTextNode(text));
      box.appendChild(h);
      box.appendChild(p);
      if (button) {
        var b = document.createElement('button');
        b.className = 'btn primary';
        b.appendChild(document.createTextNode(button.label));
        b.addEventListener('click', button.click);
        box.appendChild(b);
      }
      document.body.appendChild(box);
    };
    if (document.body) draw(); else document.addEventListener('DOMContentLoaded', draw);
  }

  function webviewTooOld() {
    show('Update Android System WebView',
      'The launcher needs a newer Android System WebView. Update it in the Play Store (your version: ' +
      chromium + ', needed: ' + MIN_CHROMIUM + '), then open Downhill Revived again.',
      { label: 'Open the Play Store', click: function () { openLink('webview'); } });
  }

  boot.failed = function (error) {
    if (boot.started) return;
    if (android && chromium && chromium < MIN_CHROMIUM) return webviewTooOld();
    var what = error && error.message ? error.message : String(error);
    show('The launcher could not start', what + (android ? ' (Android System WebView ' + chromium + ')' : ''));
  };

  if (android && chromium && chromium < MIN_CHROMIUM) webviewTooOld();
  window.addEventListener('error', function (e) { boot.failed(e.error || e.message); });
  window.addEventListener('unhandledrejection', function (e) { boot.failed(e.reason); });
  return boot;
})();

if (typeof module !== 'undefined') module.exports = dhdBoot;
