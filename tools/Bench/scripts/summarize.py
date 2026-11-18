"""One line per se_bench output file in a directory (see interleave.sh).

    python tools/Bench/scripts/summarize.py <out-dir>

adjacency: ms per edit, normals region time, parallel regions and stroke hash per brush.
live:      inputs queued, scratch, dab latency p50/p95, lock hold p50, reader wait p95/max, undo/redo.
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
        out.append('%s %.2f%s hash %s' % (m[1], float(m[3]), region, m[4][:8]))
    return ' | '.join(out)


def live(t):
    out = []
    for b in ('Draw', 'Smooth', 'Move'):
        m = re.search(r'  %s: .*?inputs queued max (\d+).*?scratch ([\d.]+) MiB' % b, t)
        if m:
            out.append('%s queued %s scratch %s' % (b, m[1], m[2]))
    dab = re.findall(r'dab latency \(gather to publish\)\s+p50\s+([\d.]+)\s+p95\s+([\d.]+)', t)
    hold = re.findall(r'lock hold per dab\s+p50\s+([\d.]+)', t)
    rw = re.findall(r'reader wait \(every frame\)\s+p50\s+[\d.]+\s+p95\s+([\d.]+)\s+max\s+([\d.]+)', t)
    ur = re.search(r'undo ([\d.]+) ms \(lock hold ([\d.]+)\), redo ([\d.]+) ms \(lock hold ([\d.]+)\)', t)
    s = '; '.join(out)
    s += ' | dab p50/p95 ' + ' '.join('%s/%s' % d for d in dab)
    s += ' | hold p50 ' + ' '.join(hold)
    s += ' | reader wait p95/max ' + ' '.join('%s/%s' % r for r in rw)
    if ur:
        s += ' | undo %s (hold %s) redo %s (hold %s)' % ur.groups()
    return s


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
        if 'live strokes at level' in t:
            body = live(t)
        elif 'hash after the cycles' in t:
            body = history(t)
        else:
            body = adjacency(t)
        print('%s :: %s :: %s' % (os.path.basename(f)[:-4], body, proc(t)))


if __name__ == '__main__':
    main()
