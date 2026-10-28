(() => {
    'use strict';
    if (window !== window.top) return;
    const queue = [];
    let bytes = 0, overflow = false;
    Object.defineProperty(window, 'TelegramWebviewProxy', {value: Object.freeze({
        postEvent(type, data) {
            if (typeof type !== 'string' || type.length > 64 ||
                typeof data !== 'string' || data.length > 8192 ||
                queue.length >= 64 || bytes + data.length > 65536) {
                overflow = true;
                return;
            }
            queue.push({type, data});
            bytes += data.length;
        }
    })});
    Object.defineProperty(window, '__vitagramDrain', {value() {
        const result = JSON.stringify({events: queue, overflow});
        queue.length = 0;
        bytes = 0;
        overflow = false;
        return result;
    }});
})();
