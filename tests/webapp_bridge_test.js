const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(`${__dirname}/../assets/webapp-bridge.js`, 'utf8');
function load(top = true) {
    const window = {};
    window.top = top ? window : {};
    vm.runInNewContext(source, {window});
    return window;
}
const window = load();
window.TelegramWebviewProxy.postEvent('web_app_ready', '{}');
window.TelegramWebviewProxy.postEvent('web_app_data_send', JSON.stringify({data: '👨‍👩‍👧‍👦\n"\\'}));
let batch = JSON.parse(window.__vitagramDrain());
assert.equal(batch.overflow, false);
assert.equal(batch.events.length, 2);
assert.equal(JSON.parse(batch.events[1].data).data, '👨‍👩‍👧‍👦\n"\\');
// Telegram's SDK serializes payload-free events as an empty JSON string.
window.TelegramWebviewProxy.postEvent('web_app_request_theme', JSON.stringify(''));
assert.equal(JSON.parse(window.__vitagramDrain()).events[0].data, '""');
assert.deepEqual(JSON.parse(window.__vitagramDrain()), {events: [], overflow: false});
for (let i = 0; i < 65; ++i) window.TelegramWebviewProxy.postEvent('event', '{}');
batch = JSON.parse(window.__vitagramDrain());
assert.equal(batch.events.length, 64);
assert.equal(batch.overflow, true);
window.TelegramWebviewProxy.postEvent('event', 'x'.repeat(8193));
assert.deepEqual(JSON.parse(window.__vitagramDrain()), {events: [], overflow: true});
window.TelegramWebviewProxy.postEvent('event', {});
assert.equal(JSON.parse(window.__vitagramDrain()).overflow, true);
assert.equal(load(false).TelegramWebviewProxy, undefined);
assert.throws(() => Object.defineProperty(window, '__vitagramDrain', {value() {}}));
const largest = load();
for (let i = 0; i < 8; ++i) largest.TelegramWebviewProxy.postEvent('\0'.repeat(64), '\0'.repeat(8192));
const encoded = largest.__vitagramDrain();
assert.equal(JSON.parse(encoded).overflow, false);
assert.ok(encoded.length * 3 + 1 < 2 * 1024 * 1024); // WKString's maximum UTF-8 allocation.
console.log('Web App bridge: top-frame isolation, exact data, bounded queue, overflow and draining passed');
