/* ═══════════════════════════════════════════════════════════════════════
   Chart.js global defaults for M3 dark theme
   ═══════════════════════════════════════════════════════════════════ */
Chart.defaults.color = '#C2C9BD';
Chart.defaults.borderColor = '#42493F';
Chart.defaults.font.family = "'Google Sans Text', system-ui, sans-serif";

/* ═══════════════════════════════════════════════════════════════════════
   Shared helpers
   ═══════════════════════════════════════════════════════════════════ */
function clearChildren(el) { while (el.firstChild) el.removeChild(el.firstChild); }

function createStatCard(label, value, cssClass) {
    const card = document.createElement('div');
    card.className = 'stat-card';
    const labelEl = document.createElement('div');
    labelEl.className = 'label';
    labelEl.textContent = label;
    const valueEl = document.createElement('div');
    valueEl.className = 'value' + (cssClass ? ' ' + cssClass : '');
    valueEl.textContent = value;
    card.appendChild(labelEl);
    card.appendChild(valueEl);
    return card;
}

function createTransferRow(t) {
    const tr = document.createElement('tr');
    const ts = t.start ? new Date(t.start * 1000).toLocaleString() : '-';
    [
        t.session_id.substring(0, 8) + '...',
        ts,
        (t.size_bytes / 1024).toFixed(1) + ' KB',
        String(t.chunks),
        (t.end - t.start).toFixed(1) + 's',
        (t.goodput_bps / 1000).toFixed(2) + ' kbps',
        String(t.retransmits)
    ].forEach(text => {
        const td = document.createElement('td');
        td.textContent = text;
        tr.appendChild(td);
    });
    return tr;
}
