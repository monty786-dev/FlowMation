/* FlowBrowser UI - start-up guard. Loaded first by every UI page, before compat.js.
   On WebView2 / Chromium it does nothing. In the Wine / Gecko build (Internet-Explorer style engine) it
     * tells common.js to use the polling transport (no ?query on the address is needed),
     * collects script errors and writes them to host.log (the host polls window.__fbPullDiag()),
     * shows a visible message instead of a blank white page when a page throws or does not finish starting.
   It is written in the oldest JavaScript on purpose (no ES5 library calls), so it still runs when everything else fails. */
(function () {
  var W = window, D = document;
  var legacy = false;
  try { legacy = !!(D.documentMode || /Trident|MSIE/.test(navigator.userAgent || '') || /[?&]host=ie(&|$)/.test(String(location.search || ''))); } catch (e0) { legacy = false; }
  if (!legacy) { W.__fbReport = function (m) { try { if (W.console && W.console.error) W.console.error(String(m)); } catch (e1) { } }; return; }

  var queue = [], errors = [], box = null;
  W.__fbHostIE = 1;
  function say(s) { queue[queue.length] = String(s); }
  W.__fbPullDiag = function () { if (!queue.length) return ''; var s = queue.join('\n'); queue = []; return s; };

  function env() {
    var r = [];
    try { r[r.length] = 'document mode ' + (D.documentMode || '?') + ', ' + D.readyState; } catch (e1) { }
    try { r[r.length] = 'JSON:' + (typeof W.JSON) + ' Promise:' + (typeof W.Promise) + ' Object.keys:' + (typeof Object.keys) + ' bind:' + (typeof Function.prototype.bind) + ' forEach:' + (typeof Array.prototype.forEach); } catch (e2) { }
    try { r[r.length] = 'addEventListener:' + (typeof W.addEventListener) + ' querySelector:' + (typeof D.querySelector) + ' classList:' + ((D.documentElement && D.documentElement.classList) ? 'yes' : 'no'); } catch (e3) { }
    try { r[r.length] = 'stylesheets:' + (D.styleSheets ? D.styleSheets.length : '?') + ' body children:' + (D.body ? D.body.childNodes.length : 'no body'); } catch (e4) { }
    try { r[r.length] = navigator.userAgent; } catch (e5) { }
    return r.join('\n');
  }

  function show(head) {
    try {
      var b = D.body;
      if (!b) { setTimeout(function () { show(head); }, 150); return; }
      if (!box) {
        box = D.createElement('pre');
        box.id = 'fb-diag';
        box.style.cssText = 'position:fixed;left:0;top:0;right:0;z-index:2147483647;margin:0;padding:10px 12px;max-height:70%;overflow:auto;' +
          'background:#fffbe6;color:#7a1f00;border-bottom:3px solid #e95420;font:12px/1.4 Consolas,monospace;white-space:pre-wrap;word-wrap:break-word;';
        b.appendChild(box);
      }
      box.innerHTML = '';
      box.appendChild(D.createTextNode(head + '\n\n' + errors.join('\n') + (errors.length ? '\n\n' : '') + env() + '\n\nThe same text is in logs\\host.log.'));
    } catch (e6) { }
  }

  W.__fbReport = function (m) {
    errors[errors.length] = String(m);
    say('error: ' + m);
    show('FlowBrowser hit a problem while starting this page:');
  };
  W.onerror = function (msg, url, line) {
    var f = String(url || '');
    f = f.substring(f.lastIndexOf('/') + 1);
    W.__fbReport(msg + '  (' + f + ' line ' + line + ')');
    return false;
  };

  say('page start ' + location.href + '\n' + env());
  W.setTimeout(function () {
    if (W.__fbBooted) return;
    say('page did not finish starting within 6 s\n' + env());
    show('FlowBrowser has not finished starting this page (still waiting for the app):');
  }, 6000);
  /* a slow start is not an error: when the page does come up and nothing was thrown, the notice goes away again */
  W.setInterval(function () {
    if (W.__fbBooted && box && !errors.length) { try { box.parentNode.removeChild(box); } catch (e7) { } box = null; }
  }, 500);
})();
