async function sendNodeCmd(nodeId, cmdId) {
    const r = await fetch('/api/cmd/'+encodeURIComponent(nodeId), {
        method:'POST', credentials:'same-origin',
        headers:{'Content-Type':'application/json'},
        body: JSON.stringify({cmd:cmdId})
    });
    if (!r.ok) { alert('Command failed: '+r.status); return; }
    const data = await r.json();
    if (data.ok) console.log('Command sent:', data);
}
