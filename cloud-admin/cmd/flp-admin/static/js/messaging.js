async function sendTopicMsg() {
    const nodeId = document.getElementById('msg-node-select').value;
    const topic = document.getElementById('msg-topic').value.trim();
    if (!topic) { alert('Topic is required'); return; }

    const msgText = document.getElementById('msg-text').value.trim();
    const payloadHex = document.getElementById('msg-payload').value.trim();

    if (!msgText && !payloadHex) {
        alert('Either message text or hex data is required');
        return;
    }
    if (msgText && payloadHex) {
        alert('Specify either message text OR hex data, not both');
        return;
    }

    const body = { topic };
    if (msgText) {
        body.message = msgText;
    } else {
        body.data = payloadHex;
    }

    const r = await fetch('/api/topic_msg/'+encodeURIComponent(nodeId), {
        method:'POST', credentials:'same-origin',
        headers:{'Content-Type':'application/json'},
        body: JSON.stringify(body)
    });
    if (!r.ok) { alert('Request failed: '+r.status); return; }
    const data = await r.json();

    const log = document.getElementById('msg-log');
    const ts = new Date().toLocaleTimeString();
    const entry = document.createElement('div');
    let content = '';
    if (msgText) {
        content = `[${ts}] → 0x${nodeId} topic="${topic}" message="${msgText}" (${data.bytes} bytes)`;
    } else {
        content = `[${ts}] → 0x${nodeId} topic="${topic}" data=${payloadHex} (${data.bytes} bytes)`;
    }
    if (!data.ok) {
        content += ` ERROR: ${data.error||'unknown'}`;
        entry.style.color = '#FFB4AB';
    } else {
        entry.style.color = '#66BB6A';
    }
    entry.textContent = content;
    log.insertBefore(entry, log.firstChild);

    document.getElementById('msg-text').value = '';
    document.getElementById('msg-payload').value = '';
}
