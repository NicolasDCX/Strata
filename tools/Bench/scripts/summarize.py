"""One line per se_bench output file in a directory (see interleave.sh).

    python tools/Bench/scripts/summarize.py <out-dir>

adjacency: ms per edit, brush time (Smooth by stage), normals region time, gather by stage, brush scratch, parallel regions and stroke hash per brush.
live:      inputs queued (max and at release), time to drain after the last input, scratch, dab latency p50/p95/max, lock hold p50, reader wait p95/max, undo/redo.
rates:     per stroke (brush, radius, input rate): dabs, dabs per input, queue growth and drain, queue wait,
           processing, input-to-visible and dab latency p50/p95/max, reader wait; with latest-wins Move
           updates also applied/coalesced inputs, every input's visibility and release to visible.
history:   undo/redo lock hold, total and edit-region time (p50 per scenario), hashes.
Every file: peak working set, peak commit and lowest free RAM when run through peakrun.ps1.
The kind is detected from the file's contents, so file names are free.
"""
import glob
import os
import re
import sys


def proc(t):
    m = re.search(r'peak working set ([\d,.]+) GiB, peak private commit ([\d,.]+) GiB, lowest free RAM ([\d,.]+)', t)
    return 'ws %s commit %s free %s' % m.groups() if m else ''


def adjacency(t):
    out = []
    lines = t.split('\n')
    for l in lines:
        m = re.match(r'^  (Draw|Smooth|Move) +(\d+) edits,\s+([\d.]+) ms/edit.*hash (\w+)', l)
        if not m:
            continue
        region = ''
        # Log lines can interleave, so find the normals line by its edit count.
        for k in lines:
            n = re.search(r'normals per edit [\d.]+: region ([\d.]+).*?(\d+) of (%s) regions parallel' % m[2], k)
            if n:
                region = ' region %.2f par %s/%s' % (float(n[1]), n[2], n[3])
                break
        gather = ''
        if m[1] != 'Move':
            # Find the gather line by the gather total in the brush line: query,
            # select, record, normal and the record + normal + staging wall.
            total = re.search(r' gather ([\d.]+) ', l)
            g = total and re.search(r'gather per edit (%s): query ([\d.]+) \+ select ([\d.]+) \+ undo record ([\d.]+) '
                                    r'\+ average normal ([\d.]+).*?wall ([\d.]+) ms' % re.escape(total[1]), t)
            if g:
                gather = ' gather %.2f (q %.2f s %.2f rec %.2f nrm %.2f wall %.2f)' % tuple(float(x) for x in g.groups())
        brush = re.search(r'\(brush ([\d.]+) ', l)
        extra = ' brush %.2f' % float(brush[1]) if brush else ''
        if m[1] == 'Smooth':
            sm = re.search(r'smooth brush per dab: grid clear [\d.]+, bin ([\d.]+), occupied [\d.]+, kernel ([\d.]+), '
                           r'kernel commit ([\d.]+), Laplacian ([\d.]+), Laplacian commit ([\d.]+) ms; .*?(\d+) binned in parallel', t)
            if sm:
                extra += ' (bin %.2f kernel %.2f+%.2f lap %.2f+%.2f, %s par)' % (
                    float(sm[1]), float(sm[2]), float(sm[3]), float(sm[4]), float(sm[5]), sm[6])
        out.append('%s %.2f%s%s%s hash %s' % (m[1], float(m[3]), extra, region, gather, m[4][:8]))
    bs = re.search(r'brush scratch \(DabScratch[^)]*\): ([\d.]+) MiB', t)
    if bs:
        out.append('brush scratch %s MiB' % bs[1])
    return ' | '.join(out)


def live(t):
    out = []
    # Log lines can interleave between brushes: a dab-latency line belongs to
    # the brush whose dab count equals its sample count (n).
    dab = {n: (p50, p95, mx) for p50, p95, mx, n in re.findall(
        r'dab latency \(gather to publish\)\s+p50\s+([\d.]+)\s+p95\s+([\d.]+)\s+max\s+([\d.]+) ms\s+\(n (\d+)\)', t)}
    for b in ('Draw', 'Smooth', 'Move'):
        m = re.search(r'  %s: .*?\(fed in ([\d.]+) ms\), (\d+) dabs, stroke done after ([\d.]+) ms; inputs queued max (\d+), at release (\d+).*?scratch ([\d.]+) MiB' % b, t)
        if m:
            d = dab.get(m[2])
            out.append('%s dab p50/p95/max %s queued max/release %s/%s drain %.0f ms scratch %s' % (
                b, '/'.join(d) if d else '?', m[4], m[5], float(m[3]) - float(m[1]), m[6]))
    hold = re.findall(r'lock hold per dab\s+p50\s+([\d.]+)', t)
    rw = re.findall(r'reader wait \(every frame\)\s+p50\s+[\d.]+\s+p95\s+([\d.]+)\s+max\s+([\d.]+)', t)
    ur = re.search(r'undo ([\d.]+) ms \(lock hold ([\d.]+)\), redo ([\d.]+) ms \(lock hold ([\d.]+)\)', t)
    s = '; '.join(out)
    s += ' | hold p50 ' + ' '.join(hold)
    s += ' | reader wait p95/max ' + ' '.join('%s/%s' % r for r in rw)
    if ur:
        s += ' | undo %s (hold %s) redo %s (hold %s)' % ur.groups()
    return s


def rates(t):
    # One line per stroke: brush, radius, rate, then the per-input and per-dab spreads.
    out = []
    # One line per stroke (a two-line form came apart when stdout was captured).
    for head, body in re.findall(r'  RATE (.*?) \| (queue_wait .*)', t):
        h = dict(kv.split('=') for kv in head.split()[1:])
        brush = head.split()[0]
        b = dict(zip(body.split()[0::2], body.split()[1::2]))
        out.append('\n  %-6s r=%s %4s Hz: dabs %s (%.2f per input, max %s), %s dabs/s, queued max/release %s/%s, drain %s ms | '
                   'wait %s | process %s (no dab %s) | visible %s | dab %s | reader wait p95/max %s' % (
                       brush, h['r'], h['hz'], h['dabs'], int(h['dabs']) / max(1, int(h['inputs'])), h['max_dabs_per_input'],
                       h['dabs_per_s'], h['queued_max'], h['queued_release'], h['drain_ms'], b['queue_wait'], b['process'],
                       b['process_no_dab'], b['visible'], b['dab'], b['reader_wait']))
        if 'coalesced' in h:
            # Latest-wins Move updates: applied and dropped inputs, every input's
            # visibility through the input that covers it, final release to visible.
            out.append('
         applied %s, coalesced %s | visible (every input, via the input covering it) %s | '
                       'release to visible %s ms | final mesh %s' % (
                           h['applied'], h['coalesced'], b.get('visible_cover', '-'), h['release_visible_ms'], h['hash']))
    return ''.join(out)


def history(t):
    out = []
    for c in ('undo', 'redo'):
        h = re.findall(r'%s lock hold\s+p50\s+([\d.]+)' % c, t)
        tot = re.findall(r'%s total \(sculpt thread\)\s+p50\s+([\d.]+)' % c, t)
        reg = re.findall(r'%s  before: edit region\s+p50\s+([\d.]+)' % c, t)
        out.append('%s hold %s total %s region %s' % (c, '/'.join(h), '/'.join(tot), '/'.join(reg) or '-'))
    hs = re.findall(r'hash after the cycles (\w+)', t)
    return ' | '.join(out) + ' | hash ' + ' '.join(x[:8] for x in hs)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    for f in sorted(glob.glob(os.path.join(d, '*.txt'))):
        t = open(f, encoding='utf-8', errors='replace').read().replace('\r', '')
        if 'rate strokes at level' in t:
            body = rates(t)
        elif 'live strokes at level' in t:
            body = live(t)
        elif 'hash after the cycles' in t:
            body = history(t)
        else:
            body = adjacency(t)
        print('%s :: %s :: %s' % (os.path.basename(f)[:-4], body, proc(t)))


if __name__ == '__main__':
    main()
