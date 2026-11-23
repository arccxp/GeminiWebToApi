#include "gemini_web/relay.h"

#include <mutex>
#include <utility>

namespace gwa::relay {
namespace {

std::mutex g_driverMutex;
RelayDriver g_driver;

// Reference browser DOM driver script for models served over processSession.
// Selects the model row via data-test-id="bard-mode-option-<id>", sets the
// thinking level submenu, injects prompt text into the Quill editor, submits,
// and streams formatted response markdown (including KaTeX/code blocks).
constexpr const char* kDriverScriptTemplate = R"JS(
(async () => {
  const askId = %ASKID%;
  const post = (event, payload) => window.chrome.webview.postMessage("#" + askId + ":" + event + "\n" + payload);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const txt = (node) => (node.innerText || '').replace(/\s+/g, ' ').trim();
  const prompt = %PROMPT%;
  const rowId = %ROW%;
  const rowName = %ROWNAME%;
  const level = %LEVEL%;
  const fresh = %FRESH%;
  const thread = %THREAD%;
  const hasImages = %HAS_IMAGES%;
  window.__relayAbort = false;
  window.__relayGo = false;
  const t0 = Date.now();
  const stage = (name) => post('stage', name + ' +' + (Date.now() - t0) + 'ms');

  try {
    const stop = document.querySelector('button[aria-label="Stop response"]');
    if (stop) {
      stop.click();
      await sleep(900);
      stage('stopped-stale-turn');
    }
    const inThread = () => document.querySelector('model-response') !== null ||
                           /\/app\/[0-9a-f]{8,}/.test(location.pathname);

    const onRemembered = () => thread !== '' && location.pathname === thread;
    if ((fresh || !onRemembered()) && inThread()) {
      const link = document.querySelector('[data-test-id="new-chat-button"] a, a[aria-label="New chat"]') ||
                   [...document.querySelectorAll('a')].find((a) => txt(a) === 'New chat');
      if (link) {
        link.click();
        for (let i = 0; i < 24 && inThread(); i++) await sleep(250);
        await sleep(700);
      }
    }
    stage(fresh ? 'newchat' : (onRemembered() ? 'continue ' + thread : 'newchat'));

    const pickerBtn = () => [...document.querySelectorAll('button')]
      .find((b) => (b.getAttribute('aria-label') || '').startsWith('Open mode picker'));
    const openPicker = async () => {
      if (!document.querySelector('[data-test-id^="bard-mode-option-"]')) {
        const button = pickerBtn();
        if (!button) throw new Error('mode picker not found');
        button.click();
        await sleep(800);
      }
    };
    const closeAll = async () => {
      for (let i = 0; i < 3 && document.querySelector('[role=menu]'); i++) {
        [...document.querySelectorAll('.cdk-overlay-backdrop')].forEach((b) => b.click());
        await sleep(350);
      }
    };
    const pickerState = () => {
      const button = pickerBtn();
      const label = button ? (button.getAttribute('aria-label') || '') : '';
      const current = label.match(/currently\s+(.+)$/i);
      const parts = current ? current[1].trim().split(/\s+/) : [];
      return {
        row: (parts[0] || '').toLowerCase(),
        level: parts.length > 1 ? (parts[parts.length - 1] || '').toLowerCase() : '',
        label: label,
      };
    };
    const shortName = (name) => (name || '').replace(/^\d+(\.\d+)?\s+/, '').toLowerCase();

    let state = pickerState();
    const wantRow = shortName(rowName);
    const visibleOptions = () =>
      [...document.querySelectorAll('[data-test-id^="bard-mode-option-"]')]
        .filter((item) => item.getClientRects().length > 0);
    if (rowId || rowName) {
      if (wantRow && state.row === wantRow) {
        stage('row-already');
      } else {
        await openPicker();
        const options = visibleOptions();
        if (options.length === 0) throw new Error('mode picker menu did not open');
        let row = rowId
          ? options.find((item) => item.getAttribute('data-test-id') === 'bard-mode-option-' + rowId)
          : null;
        if (!row && rowName) {
          const want = rowName.toLowerCase();
          row = options.find((item) => txt(item).toLowerCase().includes(want)) || null;
        }
        if (!row) {
          throw new Error('row not in picker: ' + rowId + ' / ' + rowName);
        }
        row.click();
        await sleep(1100);
        state = pickerState();
      }
    }
    if (level && state.level !== level) {
      await openPicker();
      const thinking = [...document.querySelectorAll('[role=menuitem]')]
        .find((item) => txt(item).startsWith('Thinking level'));
      if (thinking) {
        thinking.click();
        await sleep(650);
        const menus = [...document.querySelectorAll('[role=menu]')];
        const items = menus.length
          ? [...menus[menus.length - 1].querySelectorAll('[role=menuitemradio]')]
          : [];
        const target = items.find((item) => txt(item).toLowerCase().startsWith(level));
        if (target) { target.click(); await sleep(650); }
      }
    }
    await closeAll();

    if (hasImages) {
      const upload = [...document.querySelectorAll('button')]
        .find((b) => /upload and tools/i.test(b.getAttribute('aria-label') || ''));
      if (upload && !document.querySelector('input.hidden-file-input[accept*="image"]')) {
        upload.click();
        await sleep(1100);
      }
      post('upload-ready', '1');
      const waitStarted = Date.now();
      while (!window.__relayGo && Date.now() - waitStarted < 30000) await sleep(150);
      await closeAll();
      await sleep(1200);
    }

    const findEditor = () => {
      for (const selector of ['rich-textarea .ql-editor', 'rich-textarea [contenteditable="true"]',
                              'div[contenteditable="true"]', 'textarea']) {
        const node = document.querySelector(selector);
        if (node) return node;
      }
      return null;
    };
    const editor = findEditor();
    if (!editor) throw new Error('composer not found');

    editor.focus();
    if (editor.tagName === 'TEXTAREA') {
      editor.value = prompt;
      editor.dispatchEvent(new Event('input', { bubbles: true }));
    } else {
      const range = document.createRange();
      range.selectNodeContents(editor);
      const selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      document.execCommand('insertText', false, prompt);
    }
    await sleep(400);

    const before = document.querySelectorAll('model-response').length;
    const findSend = () => {
      for (const selector of ['button[aria-label="Send message"]', 'button[aria-label*="Send"]',
                              'button.send-button']) {
        const node = document.querySelector(selector);
        if (node) return node.closest('button') || node;
      }
      return null;
    };
    for (let i = 0; i < 50; i++) {
      const candidate = findSend();
      if (candidate && candidate.getAttribute('aria-disabled') !== 'true' && !candidate.disabled) break;
      await sleep(250);
    }
    const send = findSend();
    if (send) send.click();
    else editor.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', code: 'Enter', keyCode: 13, which: 13, bubbles: true }));

    let last = '';
    let stable = 0;
    const started = Date.now();
    const generating = () => document.querySelector('button[aria-label="Stop response"]') !== null;
    let sawStop = false;
    const imageNames = %IMAGENAMES%;

    const answerText = (root) => {
      const clone = root.cloneNode(true);
      const mathHost = '.katex, mjx-container, .math-inline, .math-block, .math-display';
      const displayMath = '.katex-display, .math-block, .math-display, mjx-container[display="true"], math[display="block"]';
      const replaceMath = (host, source) => {
        if (!host || !clone.contains(host) || host.closest('pre, code, code-block')) return;
        let tex = (source || '').trim();
        if (!tex) return;
        const displayParent = host.closest(displayMath);
        let display = !!displayParent || !!host.querySelector('math[display="block"]');
        for (const [open, close, block] of [['$$','$$',true], ['\\[','\\]',true], ['\\(','\\)',false], ['$','$',false]]) {
          if (tex.length > open.length + close.length && tex.startsWith(open) && tex.endsWith(close)) {
            tex = tex.slice(open.length, -close.length).trim();
            display = display || block;
            break;
          }
        }
        if (!tex) return;
        const wrapper = host.closest('.katex-display');
        const target = wrapper && clone.contains(wrapper) ? wrapper : host;
        const value = display ? '\n\\[' + tex + '\\]\n' : '\\(' + tex.replace(/[\r\n]+/g, ' ') + '\\)';
        if (target === clone) clone.textContent = value;
        else target.replaceWith(document.createTextNode(value));
      };
      clone.querySelectorAll('annotation[encoding="application/x-tex" i]').forEach((node) => {
        replaceMath(node.closest(mathHost) || node.closest('math'), node.textContent);
      });
      clone.querySelectorAll('.math-inline[data-math], .math-block[data-math], .math-display[data-math], [data-latex], math[alttext]').forEach((node) => {
        replaceMath(node.closest(mathHost) || node, node.getAttribute('data-latex') || node.getAttribute('data-math') || node.getAttribute('alttext'));
      });
      clone.querySelectorAll('button, gem-icon-button, mat-icon, .code-block-decoration, .buttons, .code-block-footer, .response-footer, .sources-list, [aria-hidden="true"]').forEach((node) => node.remove());
      clone.querySelectorAll('code-block').forEach((block) => {
        const code = block.querySelector('code[data-test-id="code-content"]') || block.querySelector('code');
        const body = ((code ? code.textContent : block.textContent) || '').trim();
        block.replaceWith(document.createTextNode('\n```\n' + body + '\n```\n'));
      });
      clone.querySelectorAll('code').forEach((node) => {
        if (node.closest('pre')) return;
        const body = node.textContent || '';
        if (!body) return;
        const runs = body.match(/\x60+/g) || [];
        const marker = '\x60'.repeat(runs.reduce((max, run) => Math.max(max, run.length), 0) + 1);
        const pad = body.startsWith('\x60') || body.endsWith('\x60') ? ' ' : '';
        node.replaceWith(document.createTextNode(marker + pad + body + pad + marker));
      });
      clone.querySelectorAll('p, li, h1, h2, h3, h4, tr, br').forEach((node) => {
        if (node.tagName === 'BR') node.replaceWith(document.createTextNode('\n'));
        else node.appendChild(document.createTextNode('\n'));
      });
      return clone.textContent || '';
    };

    const bodyOf = (raw) => {
      let out = raw.replace(/^Gemini said\s*/i, '');
      for (const name of imageNames) {
        if (name) out = out.split(name).join(' ');
      }
      return out.replace(/[ \t]+/g, ' ').replace(/[ \t]+\n/g, '\n').replace(/\n{3,}/g, '\n\n').trim();
    };

    while (Date.now() - started < %LIMIT_MS%) {
      if (window.__relayAbort) { post('error', 'aborted'); return; }
      await sleep(300);
      const nodes = document.querySelectorAll('model-response');
      if (nodes.length <= before) continue;
      const bodyNode = nodes[nodes.length - 1].querySelector('message-content .markdown') ||
                       nodes[nodes.length - 1].querySelector('.markdown') ||
                       nodes[nodes.length - 1].querySelector('message-content') ||
                       nodes[nodes.length - 1];
      const text = bodyOf(answerText(bodyNode));
      if (generating()) sawStop = true;
      if (!text) continue;
      if (text !== last) { last = text; stable = 0; post('delta', text); }
      else if (++stable >= (sawStop && !generating() ? 4 : 15)) break;
    }
    post('thread', location.pathname || '');
    if (last) post('done', last);
    else post('error', 'Gemini produced no answer text');
  } catch (error) {
    post('error', (error && error.message) ? error.message : String(error));
  }
})();
)JS";

constexpr const char* kPickerScript = R"JS(
(async () => {
  const post = (event, payload) => window.chrome.webview.postMessage(event + "\n" + payload);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const txt = (node) => (node.innerText || '').replace(/\s+/g, ' ').trim();
  try {
    const button = [...document.querySelectorAll('button')]
      .find((b) => (b.getAttribute('aria-label') || '').startsWith('Open mode picker'));
    if (!button) throw new Error('mode picker not found');
    if (!document.querySelector('[data-test-id^="bard-mode-option-"]')) {
      button.click();
      await sleep(1200);
    }
    const items = [...document.querySelectorAll('[data-test-id^="bard-mode-option-"]')]
      .filter((item) => item.getClientRects().length > 0);
    if (!items.length) throw new Error('mode picker menu did not open');
    for (const item of items) {
      const raw = txt(item);
      const cut = raw.search(/\bLimit\b|\bResets?\b|\bout of\b/i);
      const clean = cut >= 0 ? raw.slice(0, cut).trim() : raw;
      const note = cut >= 0 ? raw.slice(cut).trim() : '';
      const tokens = clean.split(' ');
      let at = tokens.findIndex((token) => /^[0-9]/.test(token));
      if (at < 0) at = 0;
      const category = tokens.slice(0, at).join(' ');
      const label = tokens.slice(at).join(' ');
      const id = (item.getAttribute('data-test-id') || '').replace('bard-mode-option-', '');
      const enabled = item.getAttribute('aria-disabled') !== 'true';
      post('row', id + '\t' + (enabled ? '1' : '0') + '\t' + category + '\t' + label + '\t' + note);
    }
    post('rows-done', String(items.length));
  } catch (error) {
    post('error', (error && error.message) ? error.message : String(error));
  }
})();
)JS";

}  // namespace

void SetDriver(RelayDriver driver) {
    std::lock_guard<std::mutex> lock(g_driverMutex);
    g_driver = std::move(driver);
}

bool Available(std::string& error) {
    RelayDriver driver;
    {
        std::lock_guard<std::mutex> lock(g_driverMutex);
        driver = g_driver;
    }
    if (!driver.available) {
        error = "no browser relay driver registered";
        return false;
    }
    return driver.available(error);
}

bool Started() {
    RelayDriver driver;
    {
        std::lock_guard<std::mutex> lock(g_driverMutex);
        driver = g_driver;
    }
    return driver.started ? driver.started() : false;
}

bool Ask(const CookieJar& jar, const std::string& rowId, const std::string& rowName,
         const std::string& level, const std::string& prompt,
         const std::vector<std::string>& imagePaths, bool freshChat, std::string& answer,
         std::string& error, std::string& thread,
         const std::function<void(const std::string&)>& onPartial, int timeoutMs) {
    RelayDriver driver;
    {
        std::lock_guard<std::mutex> lock(g_driverMutex);
        driver = g_driver;
    }
    if (!driver.ask) {
        error = "no browser relay driver registered";
        return false;
    }
    return driver.ask(jar, rowId, rowName, level, prompt, imagePaths, freshChat, answer, error,
                      thread, onPartial, timeoutMs);
}

bool ListModels(const CookieJar& jar, std::vector<RowInfo>& rows, std::string& error,
                int timeoutMs) {
    RelayDriver driver;
    {
        std::lock_guard<std::mutex> lock(g_driverMutex);
        driver = g_driver;
    }
    if (!driver.listModels) {
        error = "no browser relay driver registered";
        return false;
    }
    return driver.listModels(jar, rows, error, timeoutMs);
}

bool ReadCookieHeader(const CookieJar& jar, std::string& header, std::string& error,
                      int timeoutMs) {
    RelayDriver driver;
    {
        std::lock_guard<std::mutex> lock(g_driverMutex);
        driver = g_driver;
    }
    if (!driver.readCookieHeader) {
        error = "no browser relay driver registered";
        return false;
    }
    return driver.readCookieHeader(jar, header, error, timeoutMs);
}

void Shutdown() {
    RelayDriver driver;
    {
        std::lock_guard<std::mutex> lock(g_driverMutex);
        driver = g_driver;
    }
    if (driver.shutdown) driver.shutdown();
}

const char* DriverScriptTemplate() { return kDriverScriptTemplate; }
const char* PickerScript() { return kPickerScript; }

}  // namespace gwa::relay
