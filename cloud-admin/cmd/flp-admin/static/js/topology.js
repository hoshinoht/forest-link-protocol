const network = new vis.Network(
    document.getElementById('topo-container'),
    { nodes: new vis.DataSet(), edges: new vis.DataSet() },
    {
        nodes: { shape: 'dot', size: 20, font: { color: '#E2E3DD', size: 13 },
                 borderWidth: 2, shadow: { enabled: true, size: 6, color: 'rgba(0,0,0,0.3)' } },
        edges: { color: { color: '#42493F', highlight: '#A5D6A7' },
                 font: { color: '#8C9388', size: 10, align: 'top' }, arrows: { to: false }, width: 2 },
        physics: { solver: 'forceAtlas2Based', forceAtlas2Based: { gravitationalConstant: -80, springLength: 140 } },
        interaction: { hover: true }
    }
);
const topoNodes = network.body.data.nodes;
const topoEdges = network.body.data.edges;

async function pollTopology() {
    try {
        const r = await fetch('/api/topology', {credentials:'same-origin'});
        const data = await r.json();
        const nodeIds = new Set(), edgeIds = new Set();
        let online = 0, offline = 0;

        data.nodes.forEach(n => {
            nodeIds.add(n.id);
            const color = n.status === 'online' ? '#66BB6A' : n.status === 'inferred' ? '#8C9388' : '#EF5350';
            const label = n.id + '\n' + n.status;
            let title = n.id + ' (' + n.status + ')';
            if (n.heap) {
                title += '\nPSRAM: ' + (n.heap.free_psram/1024).toFixed(0) + ' KB';
                title += '\nSRAM: ' + (n.heap.free_internal/1024).toFixed(0) + ' KB';
            }
            if (topoNodes.get(n.id)) {
                topoNodes.update({id:n.id, label, title, color:{background:color, border:color}});
            } else {
                topoNodes.add({id:n.id, label, title, color:{background:color, border:color}});
            }
            if (n.status === 'online') online++; else if (n.status !== 'inferred') offline++;
            ['node-select','heap-node-select','msg-node-select'].forEach(selId => {
                const sel = document.getElementById(selId);
                if (!sel.querySelector('option[value="'+n.id+'"]')) {
                    const opt = document.createElement('option');
                    opt.value = n.id; opt.textContent = n.id;
                    sel.appendChild(opt);
                }
            });
        });

        data.edges.forEach(e => {
            const eid = [e.source,e.target].sort().join('-');
            edgeIds.add(eid);
            const label = e.rssi + 'dBm ' + e.transport;
            const edgeColor = e.transport === 'ESP-NOW' ? '#A5D6A7' : '#A1CED5';
            if (topoEdges.get(eid)) {
                topoEdges.update({id:eid, from:e.source, to:e.target, label, color:{color:edgeColor}});
            } else {
                topoEdges.add({id:eid, from:e.source, to:e.target, label, color:{color:edgeColor}});
            }
        });

        topoNodes.getIds().forEach(id => { if (!nodeIds.has(id)) topoNodes.remove(id); });
        topoEdges.getIds().forEach(id => { if (!edgeIds.has(id)) topoEdges.remove(id); });

        const s = document.getElementById('topo-stats');
        clearChildren(s);
        s.appendChild(createStatCard('Nodes Online', online, 'online'));
        s.appendChild(createStatCard('Nodes Offline', offline, 'offline'));
        s.appendChild(createStatCard('Links', data.edges.length, ''));
    } catch (e) {}
}
setInterval(pollTopology, 5000);
pollTopology();
