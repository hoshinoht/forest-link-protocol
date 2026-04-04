let heapChart = null;

async function loadHeap() {
    const nodeId = document.getElementById('heap-node-select').value;
    const range = document.getElementById('heap-range-select').value;
    if (!nodeId) return;
    const r = await fetch('/api/heap/'+encodeURIComponent(nodeId)+'?range='+range, {credentials:'same-origin'});
    const data = await r.json();
    if (heapChart) heapChart.destroy();
    if (!data.length) return;
    const labels = data.map(d=>new Date(d.timestamp*1000).toLocaleTimeString());
    const latest = data[data.length-1];
    const s = document.getElementById('heap-stats');
    clearChildren(s);
    s.appendChild(createStatCard('Free PSRAM', (latest.free_psram/1024).toFixed(0)+' KB', ''));
    s.appendChild(createStatCard('Free SRAM', (latest.free_internal/1024).toFixed(0)+' KB', ''));
    s.appendChild(createStatCard('Min PSRAM', (latest.min_psram/1024).toFixed(0)+' KB', ''));
    s.appendChild(createStatCard('Min SRAM', (latest.min_internal/1024).toFixed(0)+' KB', ''));
    s.appendChild(createStatCard('Largest Block', (latest.largest_block/1024).toFixed(0)+' KB', ''));
    heapChart = new Chart(document.getElementById('heap-chart'), {
        type:'line', data:{labels, datasets:[
            {label:'Free PSRAM (KB)',data:data.map(d=>d.free_psram/1024),borderColor:'#A5D6A7',tension:.3},
            {label:'Free SRAM (KB)',data:data.map(d=>d.free_internal/1024),borderColor:'#66BB6A',tension:.3},
            {label:'Min PSRAM (KB)',data:data.map(d=>d.min_psram/1024),borderColor:'#A5D6A7',borderDash:[5,3],tension:.3},
            {label:'Min SRAM (KB)',data:data.map(d=>d.min_internal/1024),borderColor:'#66BB6A',borderDash:[5,3],tension:.3},
            {label:'Largest Block (KB)',data:data.map(d=>d.largest_block/1024),borderColor:'#A1CED5',tension:.3},
        ]},
        options:{responsive:true,
            plugins:{legend:{labels:{color:'#C2C9BD'}}},
            scales:{x:{ticks:{color:'#8C9388'},grid:{color:'#42493F'}},
                    y:{ticks:{color:'#8C9388'},grid:{color:'#42493F'},title:{display:true,text:'KB',color:'#8C9388'}}}}
    });
}
