import json, collections, sys
sys.path.insert(0, sys.argv[1])
from linkparse import parse_link, group_parts
d = json.load(open(sys.argv[2], encoding='utf-8'))['packages']
tot=0; resid=collections.Counter(); labels=collections.Counter(); cov=collections.Counter(); err=0
st=collections.Counter(); runs=collections.Counter(); hostsets=collections.Counter()
for p in d:
    for l in p['downloadLinks']:
        tot+=1
        try: f=parse_link(l['name'],l['url'])
        except Exception as e: err+=1; continue
        if f['residue']: resid[l['name']]+=1
        if f['label']: labels[f['label']]+=1
        for k in ('host','region','kind','format','version'):
            if f[k]: cov[k]+=1
        if f['fw'] or f['backport_target']: cov['fw or bp target']+=1
        if not f['kind_inferred']: cov['kind explicit']+=1
        if f['part'] is not None or f['part_unknown']: cov['part marker']+=1
    g=group_parts([(l['index'],l['name'],l['url']) for l in p['downloadLinks']])
    for s in g['sets']:
        st[s['status']]+=1; st['sets']+=1; st['parts']+=len(s['links'])
        if s['status']=='restarts': runs[len(s['runs'])]+=1
    if g['sets']: st['packages with sets']+=1
print('links',tot,'errors',err,'with residue',sum(resid.values()))
print(resid.most_common(10))
for k,v in cov.most_common(): print(f'  {k}: {v} ({100*v/tot:.1f}%)')
print('labels', labels.most_common(40))
print(st, 'runs per restart set', runs)
