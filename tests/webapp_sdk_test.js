// Local protocol check against the official SDK; this is not a browser or Telegram login.
const vm = require('node:vm');
const fs = require('node:fs');
const assert = require('node:assert/strict');
assert.ok(process.argv[2], 'Pass a local copy of the official telegram-web-app.js SDK');

const colors = {}, storage = {};
const context = {
    console: {log() {}, warn() {}, error() {}},
    JSON, decodeURIComponent, encodeURIComponent, URL, Date, Math, setTimeout, clearTimeout,
    location: {hash: '#tgWebAppVersion=8.0&tgWebAppPlatform=VitaGram'},
    navigator: {userAgent: 'VitaGram'},
    innerHeight: 392,
    addEventListener() {}, removeEventListener() {},
    document: {
        documentElement: {style: {
            setProperty(key, value) { colors[key] = value; },
            removeProperty(key) { delete colors[key]; }
        }},
        addEventListener() {}, removeEventListener() {},
        createElement(tag) {
            if (tag.toUpperCase() === 'A') return new URL('https://example.invalid/');
            return {style: {}, appendChild() {}, setAttribute() {}};
        },
        head: {appendChild() {}}
    },
    sessionStorage: {
        getItem(key) { return storage[key] || null; },
        setItem(key, value) { storage[key] = value; }
    }
};
context.window = context.top = context.parent = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync('assets/webapp-bridge.js', 'utf8'), context);
vm.runInContext(fs.readFileSync(process.argv[2], 'utf8'), context);

const initial = JSON.parse(context.__vitagramDrain());
assert.equal(initial.overflow, false);
for (const type of ['web_app_request_theme', 'web_app_request_viewport',
                    'web_app_request_safe_area', 'web_app_request_content_safe_area']) {
    assert.ok(initial.events.some(event => event.type === type));
}
const app = context.Telegram.WebApp;
const receive = context.Telegram.WebView.receiveEvent;
receive('theme_changed', {theme_params: {
    bg_color: '#112233', text_color: '#eeeeee', button_color: '#334455', button_text_color: '#ffffff'
}});
receive('viewport_changed', {height: 200, is_state_stable: true, is_expanded: true});
receive('safe_area_changed', {top: 0, bottom: 0, left: 0, right: 0});
receive('content_safe_area_changed', {top: 0, bottom: 0, left: 0, right: 0});
assert.equal(app.viewportHeight, 200);
assert.equal(app.themeParams.bg_color, '#112233');
assert.equal(colors['--tg-theme-bg-color'], '#112233');
assert.equal(app.safeAreaInset.top, 0);

app.ready();
app.MainButton.setText('Continue').show();
app.BackButton.show();
let main = false, back = false;
app.MainButton.onClick(() => { main = true; });
app.BackButton.onClick(() => { back = true; });
receive('main_button_pressed'); receive('back_button_pressed');
assert.ok(main && back);
const data = '👩‍🚀\n"\\';
app.sendData(data);
const batch = JSON.parse(context.__vitagramDrain());
assert.equal(batch.overflow, false);
assert.ok(batch.events.some(event => event.type === 'web_app_ready'));
assert.ok(batch.events.some(event => event.type === 'web_app_setup_main_button' && JSON.parse(event.data).text === 'Continue'));
assert.ok(batch.events.some(event => event.type === 'web_app_setup_back_button' && JSON.parse(event.data).is_visible));
assert.ok(batch.events.some(event => event.type === 'web_app_data_send' && JSON.parse(event.data).data === data));
let cancelledInvoices = 0, closedEvents = 0;
const invoiceURL = 'https://t.me/$invoice_test-123=_';
app.onEvent('invoiceClosed', event => {
    assert.equal(event.url, invoiceURL); assert.equal(event.status, 'cancelled'); ++closedEvents;
});
for (let attempt = 0; attempt < 2; ++attempt) {
    app.openInvoice(invoiceURL, status => { assert.equal(status, 'cancelled'); ++cancelledInvoices; });
    const events = JSON.parse(context.__vitagramDrain()).events;
    assert.ok(events.some(event => event.type === 'web_app_open_invoice' && JSON.parse(event.data).slug === 'invoice_test-123=_'));
    receive('invoice_closed', {slug: 'invoice_test-123=_', status: 'cancelled'});
    receive('invoice_closed', {slug: 'invoice_test-123=_', status: 'cancelled'});
}
assert.equal(cancelledInvoices, 2); assert.equal(closedEvents, 2);
console.log('Official SDK: initialization, native events, exact sendData and invoice cancellation/retry passed');
