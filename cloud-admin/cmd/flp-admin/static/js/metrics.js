let metricsChart = null;

async function loadMetrics() {
    const nodeId = document.getElementById('node-select').value;
    const range = document.getElementById('range-select').value;
    if (!nodeId) return;
    const r = await fetch('/api/metrics/'+encodeURIComponent(nodeId)+'?range='+range, {credentials:'same-origin'});
    const data = await r.json();
    if (metricsChart) metricsChart.destroy();
    const espnow = data.filter(d => d.transport === 'ESP-NOW');
    const lora = data.filter(d => d.transport === 'LoRa');
    const labels = espnow.map(d => new Date(d.timestamp*1000).toLocaleTimeString());
    metricsChart = new Chart(document.getElementById('metrics-chart'), {
        type: 'line',
        data: {
            labels,
            datasets: [
                {label:'ESP-NOW TX', data:espnow.map(d=>d.tx), borderColor:'#A5D6A7', tension:.3},
                {label:'ESP-NOW RX', data:espnow.map(d=>d.rx), borderColor:'#66BB6A', tension:.3},
                {label:'ESP-NOW Fail', data:espnow.map(d=>d.fail), borderColor:'#EF5350', tension:.3},
                {label:'LoRa TX', data:lora.map(d=>d.tx), borderColor:'#A1CED5', borderDash:[5,3], tension:.3},
                {label:'LoRa Fail', data:lora.map(d=>d.fail), borderColor:'#FFB4AB', borderDash:[5,3], tension:.3},
                {label:'Latency (ms)', data:espnow.map(d=>d.latency_ms), borderColor:'#8C9388', yAxisID:'y1', tension:.3},
            ]
        },
        options: {
            responsive:true,
            plugins:{legend:{labels:{color:'#C2C9BD'}}},
            scales:{
                x:{ticks:{color:'#8C9388'},grid:{color:'#42493F'}},
                y:{position:'left',ticks:{color:'#8C9388'},grid:{color:'#42493F'},title:{display:true,text:'Packets',color:'#8C9388'}},
                y1:{position:'right',ticks:{color:'#8C9388'},grid:{drawOnChartArea:false},title:{display:true,text:'Latency (ms)',color:'#8C9388'}}
            }
        }
    });
}
