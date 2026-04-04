async function loadTransfers() {
    const r = await fetch('/api/transfers', {credentials:'same-origin'});
    const data = await r.json();
    const tbody = document.getElementById('transfer-body');
    clearChildren(tbody);
    let totalBytes = 0;
    data.forEach(t => { totalBytes += t.size_bytes; tbody.appendChild(createTransferRow(t)); });
    const s = document.getElementById('transfer-stats');
    clearChildren(s);
    s.appendChild(createStatCard('Total Transfers', data.length, ''));
    s.appendChild(createStatCard('Total Data', (totalBytes/1024/1024).toFixed(2)+' MB', ''));
    const avg = data.length ? (data.reduce((a,t)=>a+t.goodput_bps,0)/data.length/1000).toFixed(2) : '0';
    s.appendChild(createStatCard('Avg Goodput', avg+' kbps', ''));
}

async function pollActiveTransfer() {
    try {
        const r = await fetch('/api/active-transfer', {credentials:'same-origin'});
        const d = await r.json();
        const wrap = document.getElementById('active-transfer');
        if (!d.active) { wrap.style.display='none'; return; }
        wrap.style.display='block';
        const s = document.getElementById('active-stats');
        clearChildren(s);
        s.appendChild(createStatCard('File', d.filename, ''));
        s.appendChild(createStatCard('Progress', (d.progress*100).toFixed(1)+'%', ''));
        s.appendChild(createStatCard('Chunks', d.received+' / '+d.chunk_count, ''));
        s.appendChild(createStatCard('Size', (d.total_size/1024).toFixed(1)+' KB', ''));
        s.appendChild(createStatCard('Elapsed', d.elapsed_sec.toFixed(1)+'s', ''));
        document.getElementById('active-bar').style.width = (d.progress*100).toFixed(1)+'%';
    } catch (e) {}
}
setInterval(pollActiveTransfer, 2000);
pollActiveTransfer();
