"""iOS proof of concept (branch ios-poc): a Time Profiler trace of the app, summarized by symbol.
Record and export it (the game running, e.g. scripts/ios/trial.sh with KEEP=1):
  xcrun xctrace record --template "Time Profiler" --device UDID --all-processes --time-limit 12s --output t.trace
  xcrun xctrace export --input t.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' --output t.xml
Then, with each module's symbols (nm -n BINARY | grep -i ' t ' > FILE), its load address (the
<binary ... load-addr> in t.xml) and its __TEXT address (0x100000000 for the app, 0 for a dylib):
  python3 scripts/ios/profile.py t.xml BloodbornePoC app.syms,LOAD,0x100000000,app recomp.syms,LOAD,0x0,rc
The output names game functions only by their offsets (f_<offset>); it stays in out/ like any log."""
import sys, bisect, collections, xml.etree.ElementTree as ET
xmlp, proc = sys.argv[1], sys.argv[2]
mods = []  # (load, vmbase, addrs, names, label)
for spec in sys.argv[3:]:
    syms, load, vm, label = spec.split(',')
    a=[];n=[]
    for l in open(syms):
        p=l.split()
        if len(p)>=3: a.append(int(p[0],16)); n.append(p[2])
    mods.append((int(load,16), int(vm,16), a, n, label))
def sym(x):
    for load, vm, a, n, label in mods:
        o = x - load + vm
        if a and a[0] <= o < a[-1] + 0x100000:
            i = bisect.bisect_right(a, o) - 1
            if i >= 0: return n[i]
    return None
root = ET.parse(xmlp).getroot(); ids = {}
def res(e):
    r = e.get('ref')
    if r: return ids[r]
    if e.get('id'): ids[e.get('id')] = e
    for c in e: res(c)
    return e
selfc = collections.Counter(); incl = collections.Counter(); n = 0
per_thread = collections.defaultdict(collections.Counter)
for row in root.iter('row'):
    res(row)
    th = row.find('thread')
    if th is None: continue
    th = res(th); tn = th.get('fmt', '')
    if proc not in tn: continue
    tb = row.find('tagged-backtrace')
    if tb is None: continue
    tb = res(tb); bt = tb.find('backtrace'); bt = res(bt) if bt is not None else tb
    fr = [res(f) for f in bt.findall('frame')]
    names = []
    for f in fr:
        s = sym(int(f.get('addr', '0'), 16)) or f.get('name', '?')
        names.append(s)
    if not names: continue
    n += 1
    selfc[names[0]] += 1; per_thread[tn.split(' (')[0]][names[0]] += 1
    for s in set(names): incl[s] += 1
print('samples', n)
print('-- self'); [print(f'{c/n*100:5.1f}% {s[:100]}') for s, c in selfc.most_common(40)]
print('-- inclusive'); [print(f'{c/n*100:5.1f}% {s[:100]}') for s, c in incl.most_common(45)]
for t in ['BloodbornePoC', 'GXRenderThread', 'FMOD mixer thre']:
    tot = sum(per_thread[t].values()) or 1
    print('-- self in', t, tot); [print(f'   {c/tot*100:5.1f}% {s[:90]}') for s, c in per_thread[t].most_common(10)]
