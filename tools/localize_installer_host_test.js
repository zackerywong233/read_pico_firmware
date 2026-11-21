// Verify the installer localizer attaches to body-inserted elements and shadow roots.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

class FakeElement {
  constructor(shadowRoot = null) { this.shadowRoot = shadowRoot; }
  querySelectorAll() { return []; }
  getRootNode() { return document; }
}
class FakeShadowRoot {
  querySelectorAll() { return []; }
}
let observer;
class FakeObserver {
  constructor(callback) { this.callback = callback; this.targets = []; observer = this; }
  observe(target, options) { this.targets.push({ target, options }); }
}

const document = {
  body: new FakeElement(),
  querySelectorAll: () => [],
  createTreeWalker: () => ({ nextNode: () => null }),
};
const context = {
  document,
  NodeFilter: { SHOW_TEXT: 4 },
  Element: FakeElement,
  ShadowRoot: FakeShadowRoot,
  MutationObserver: FakeObserver,
  customElements: { whenDefined: () => Promise.resolve() },
};
vm.runInNewContext(fs.readFileSync('web/localize-installer.js', 'utf8'), context);

(async () => {
  await Promise.resolve();
  const bodyWatch = observer.targets.find((entry) => entry.target === document.body);
  assert(bodyWatch, 'document.body is observed');
  assert.equal(bodyWatch.options.childList, true);
  const shadowRoot = new FakeShadowRoot();
  observer.callback([{ target: document.body, addedNodes: [new FakeElement(shadowRoot)] }]);
  assert(observer.targets.some((entry) => entry.target === shadowRoot),
    'open shadow roots on newly added elements are observed');
  console.log('installer localization DOM observer tests passed');
})().catch((error) => { console.error(error); process.exitCode = 1; });
