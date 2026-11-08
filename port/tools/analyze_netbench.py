"""Summarize cross_bench.py's raw records without comparing endpoint clocks.

Writes summary.json, endpoints.csv and report.md in the collection directory.
RTTs include game/transport scheduling. Echo misses are not IP packet loss.
"""
import argparse
import csv
import json
from pathlib import Path
import re
import statistics


def chart(summaries):
    data = []
    for item in summaries:
        trial = item['trial']
        for side, endpoint_data in item['endpoints'].items():
            for kind, channel in endpoint_data.get('channels', {}).items():
                if channel['rtt_ms']:
                    data.append(dict(backend=trial['backend'], host=trial['host'],
                        turn=trial['force_turn'], side=side, kind=kind,
                        median=channel['rtt_ms']['median'], p95=channel['rtt_ms']['p95']))
    return '''<!doctype html><html><head><meta charset="utf-8"><style>
body{font-family:var(--font-sans,system-ui);color:var(--foreground);margin:0}
p{margin:10px 0;font-size:14px}small{color:var(--muted-foreground);font-size:12px}
select{font:inherit;background:var(--secondary);color:var(--foreground);border:1px solid var(--border);border-radius:5px;padding:4px}
svg{width:100%;height:370px;overflow:visible}text{fill:var(--foreground);font-family:var(--font-sans,system-ui);font-size:12px}
.axis{stroke:var(--border)}.sfu{fill:var(--chart-1)}.p2p{fill:var(--chart-2)}
</style></head><body><p><b>Same LAN · PC Ethernet / Mac Wi-Fi · VPN off</b></p>
<p><label>Gameplay channel <select id="channel"><option value="event">Reliable events (EVENT)</option><option value="state">Movement (STATE)</option></select></label></p>
<svg viewBox="0 0 700 370" role="img" aria-label="SFU and P2P gameplay round trip latency"></svg>
<small>Bars: mean of endpoint/run medians. Dots: mean p95. Lower is faster. Two repeats for automatic paths; one for forced TURN. RTT includes game scheduling. STATE echo replies can be coalesced by the newest-state queue; its timings cover completed probes only. Two humans and eight host AI; this does not measure ten human connections.</small>
<script>const data=DATA;
const svg=document.querySelector('svg'),pick=document.querySelector('select');
function draw(){const rows=[['pc',false],['mac',false],['pc',true],['mac',true]], d=data.filter(x=>x.kind===pick.value);
const max=Math.max(40,...d.map(x=>x.p95))*1.2, scale=x=>190+x/max*410;
let html='';for(let i=0;i<=4;i++){let x=190+i*410/4;html+=`<line class="axis" x1="${x}" x2="${x}" y1="12" y2="325"/><text x="${x}" y="350" text-anchor="middle">${(max*i/4).toFixed(0)} ms</text>`;}
rows.forEach(([host,turn],i)=>{let y=35+i*76;html+=`<text x="0" y="${y+8}">${host.toUpperCase()} hosts</text><text x="0" y="${y+26}">${turn?'Forced TURN':'Automatic path'}</text>`;
['sfu','p2p'].forEach((backend,j)=>{let a=d.filter(x=>x.host===host&&x.turn===turn&&x.backend===backend);if(!a.length)return;
let median=a.reduce((s,x)=>s+x.median,0)/a.length,p95=a.reduce((s,x)=>s+x.p95,0)/a.length,cy=y+j*25;
html+=`<text x="150" y="${cy+5}" text-anchor="end">${backend.toUpperCase()}</text><rect class="${backend}" x="190" y="${cy-7}" width="${scale(median)-190}" height="16" rx="3"/><circle class="${backend}" cx="${scale(p95)}" cy="${cy+1}" r="4"/><text x="${scale(p95)+9}" y="${cy+5}">${median.toFixed(1)} / ${p95.toFixed(1)}</text>`;});});svg.innerHTML=html;}
pick.onchange=draw;draw();</script></body></html>'''.replace('DATA', json.dumps(data))


def distribution(values):
    values = sorted(values)
    if not values:
        return {}
    def percentile(p):
        pos = (len(values) - 1) * p
        lo = int(pos)
        return values[lo] + (values[min(lo + 1, len(values) - 1)] - values[lo]) * (pos - lo)
    return dict(count=len(values), median=statistics.median(values), p95=percentile(.95),
                p99=percentile(.99), maximum=max(values), mean=statistics.mean(values))


def records(path):
    with path.open() as f:
        return list(csv.DictReader(f))


def endpoint(folder, side, seconds, rows, other):
    log = (folder / f'{side}-stderr.log').read_text(errors='replace')
    tx = [r for r in rows if r['event'] == 'tx']
    if not tx:
        return dict(error='no outgoing probes')
    start = min(float(r['local_ms']) for r in tx)
    end = start + seconds * 1000
    measured = [r for r in tx if start <= float(r['local_ms']) < end]
    warmup = next(r for r in rows if r['event'] == 'warmup')
    epoch_start = float(warmup['aux']) + start - float(warmup['local_ms'])
    result = dict(window_seconds=seconds, observed_tx_span_seconds=(max(float(r['local_ms']) for r in tx) - start) / 1000,
                  channels={})
    result['complete_window'] = result['observed_tx_span_seconds'] >= seconds
    for kind in ('state', 'event'):
        sent = [r for r in measured if r['kind'] == kind]
        replies = {r['seq']: r for r in rows if r['event'] == 'rtt' and r['kind'] == kind}
        delivered = {r['seq'] for r in other if r['event'] == 'rx' and r['kind'] == kind}
        matched = [replies[r['seq']] for r in sent if r['seq'] in replies]
        rtts = [float(r['value']) for r in matched]
        result['channels'][kind] = dict(
            sent=len(sent), echoed=len(matched), no_echo=len(sent) - len(matched),
            requests_not_observed=sum(r['seq'] not in delivered for r in sent),
            echo_over_5_seconds=sum(v > 5000 for v in rtts), rtt_ms=distribution(rtts),
            successive_rtt_change_ms=distribution([abs(b-a) for a,b in zip(rtts,rtts[1:])]),
            responder_processing_ms=distribution([float(r['aux']) for r in matched]))
    pattern = (r'netbench: epoch_ms=(\d+) backend=(\w+) path=(\w+) remote_scope=(\w+) '
               r'rx=(\d+) tx=(\d+) ice_rtt_ms=([\d.-]+)')
    counters = [dict(epoch=int(m[0]), backend=m[1], path=m[2], scope=m[3],
                     rx=int(m[4]), tx=int(m[5]), ice_rtt=float(m[6])) for m in re.findall(pattern, log)
                if epoch_start <= int(m[0]) <= epoch_start + seconds * 1000]
    result['paths'] = sorted({f"{r['path']}/{r['scope']}" for r in counters})
    result['ice_rtt_ms'] = distribution([r['ice_rtt'] for r in counters if r['ice_rtt'] >= 0])
    if len(counters) > 1:
        a, b = counters[0], counters[-1]
        duration = (b['epoch'] - a['epoch']) / 1000
        result['rtc_bytes_per_second'] = {k: (b[k]-a[k])/duration for k in ('rx', 'tx')}
    result['lobby_ping_ms'] = distribution([int(v) for v in re.findall(r' - ping (\d+) ms', log)])
    result['race_fps'] = distribution([int(v) for v in re.findall(r'online: race clock \d+ \((\d+) fps\)', log)])
    result['freeze_lines'] = [line for line in log.splitlines() if 'freeze:' in line]
    result['snapshot_diagnostics'] = [line for line in log.splitlines() if 'snapshot age p50/p95/p99' in line]
    result['performance'] = [line for line in log.splitlines() if line.startswith('perf: session race')]
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    args = p.parse_args()
    summaries, flat = [], []
    for trial in sorted(args.directory.glob('*/trial.json')):
        meta = json.loads(trial.read_text())
        if meta['status'] != 'collected':
            continue
        folder = trial.parent
        rows = dict(pc=records(folder/'bench.csv'), mac=records(folder/'mac-bench.csv'))
        item = dict(trial=meta, endpoints={})
        for side, other in [('pc','mac'), ('mac','pc')]:
            data = endpoint(folder, side, meta['seconds'], rows[side], rows[other])
            item['endpoints'][side] = data
            for kind, channel in data.get('channels', {}).items():
                flat.append(dict(trial=meta['label'], backend=meta['backend'], host=meta['host'],
                    forced_turn=meta['force_turn'], endpoint=side, channel=kind,
                    paths=';'.join(data['paths']), sent=channel['sent'], echoed=channel['echoed'],
                    no_echo=channel['no_echo'], requests_not_observed=channel['requests_not_observed'],
                    **{f'rtt_{k}_ms':v for k,v in channel['rtt_ms'].items() if k!='count'},
                    **{f'rtc_{k}_bytes_s':v for k,v in data.get('rtc_bytes_per_second',{}).items()}))
        summaries.append(item)
    (args.directory/'summary.json').write_text(json.dumps(summaries, indent=2)+'\n')
    if flat:
        fields = list(dict.fromkeys(k for row in flat for k in row))
        with (args.directory/'endpoints.csv').open('w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader(); writer.writerows(flat)
    lines = ['# PC/Mac gameplay transport benchmark', '',
        'Native Windows Ethernet / native macOS Wi-Fi. VPN disabled. Same LAN.', '',
        '60 FPS, same core and seed, two humans plus eight host AI. Both gameplay channels '
        'carry 5 probes/second per endpoint after a ten-second warmup. RTT uses only the '
        'originating monotonic clock and includes transport/game scheduling. STATE probes '
        'are 116 bytes; EVENT probes are 98 bytes. 60-second windows per direction/channel.', '',
        'Echo misses measure application delivery, not raw IP loss. Reliable EVENT retransmissions '
        'can hide network loss. SFU ICE RTT measures its server hop; it is not peer-to-peer RTT. '
        'RTC byte counters exclude TURN/TCP/TLS framing. Lobby ping and snapshot age are separate diagnostics.', '',
        'STATE probe caveat: requests and replies share the reserved newest-state queue key '
        'with each endpoint\'s own probes. They can coalesce before transmission. Missing '
        'STATE replies therefore cannot establish network loss or rider snapshot loss. '
        'Use reliable EVENT probes as the primary RTT comparison; STATE RTT covers completed echoes only.', '',
        '| Trial | Origin | Channel | Path at origin | RTT median / p95 / max ms | Echoes |',
        '|---|---|---|---|---|---|']
    for row in flat:
        lines.append(f"| {row['trial']} | {row['endpoint']} | {row['channel']} | {row['paths']} | "
            f"{row.get('rtt_median_ms',0):.1f} / {row.get('rtt_p95_ms',0):.1f} / {row.get('rtt_maximum_ms',0):.1f} | "
            f"{row['echoed']}/{row['sent']} |")
    (args.directory/'report.md').write_text('\n'.join(lines)+'\n')
    (args.directory/'chart.html').write_text(chart(summaries), encoding='utf-8')
    print(f'{len(summaries)} trials, {len(flat)} endpoint/channel summaries')


if __name__ == '__main__':
    main()
