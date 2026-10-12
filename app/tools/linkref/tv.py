import json, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
sys.path.insert(0, sys.argv[1])
from linkparse import parse_link
d = json.load(open(sys.argv[2], encoding='utf-8'))['packages']
names = {}
for p in d:
    for l in p['downloadLinks']:
        names.setdefault(l['name'], (p['sourceName'], l['url']))
want = [
 'USA 12.xx Base - Vikingfile part 5', 'EUR 9.xx Base - Akirabox', 'JPN 11.xx Base - Rootz part 3', 'ASIA 9.xx Base - Akirabox part 2',
 'HK 10.xx Base - Rootz', 'EUR 5.00 Base - Mega', 'EUR 7.61 Base - 1fichier part 1', 'USA 13.60 Game (v01.010.000) (All DLC) - Vikingfile part 1',
 'EUR 6.xx BackPort 4.xx - Vikingfile', 'USA 7.xx BackPort 4.xx - 1fichier part 3', 'USA 9.xx BackPort 4.03 - Akirabox', 'USA 12.40 BackPort 4.xx - Rootz',
 'EUR 9.xx BackPort 7.xx - Gofile', 'EUR BackPort 4.xx - Vikingfile', 'USA 11.xx BackPort - Akirabox', 'EUR 11.xx BackPort - Vikingfile part 1',
 'EUR exFAT 01.000 Base - VIKI (exFAT)', 'USA exFAT 01.006 Base - FileK (exFAT)', 'JPN exFAT 01.060 Base - Vault (exFAT)', 'EUR exFAT 01.002 Base - AKIA (exFAT)',
 'USA exFAT 01.001 Base - DATA (exFAT)', 'JPN to EUR Languages exFAT 01.000.300 Base - FileK Std (exFAT)', 'USA to EUR Languages exFAT 01.011 Base - AKIA (exFAT)',
 'EUR exFAT 01.000 BackPort backport - AKIA Backport (exFAT)', 'USA to EUR Langs exFAT 01.008.001 BackPort backport - DATA Back',
 'exFAT v01.052 Base - Vikingfile part 1', 'exFAT v01.000.003 Base - Vikingfile', 'EUR v0.2.5.3 Base - Filecrypt part 1', 'Release 1 Base - Akirabox', 'Release 2 DLCs (PKG \u2013 Psykzhack) - Akirabox part 1',
 'USA 10.xx DLC (@Pkykzhack) - Vikingfile', 'USA 10.01 DLC (@Pkykzhack) - Akirabox', 'EUR 10.xx Only DLCs (PKG \u2013 DUPLEX) - Akirabox', 'USA 9.xx Only DLCs (EMU \u2013 Pippo) - 1fichier',
 'USA 7.xx Game (v01.000)(DLC) - Rootz part 3', 'USA 12.xx Game (v01.055) (All DLCs) (APR-EMU) - Vikingfile part', 'USA 12.xx Game (v01.004.003) [APR-EMU] [ ALL DLC ] - Mediafire ',
 'EUR 12.xx Game (v01.005.000) (DLC Deluxe Edition) - Akirabox pa', 'USA 12.xx Game (v17.661.002) (DLC Premium Edition) - Akirabox p', 'USA 12.xx DLC Multiplayer - Rootz part 1',
 'USA 9.xx Update 01.003 - Akirabox', 'EUR DLC Unlocker - Vikingfile', 'USA 10.xx DLC 1 (@Pkykzhack) - Akirabox',
 '4.xx PPSA26344 \u2013 v1.008.000 \u2013 DLC \u2013 95.9 gb \u2013 4.xx \u2013 ', 'DATA (exFAT)', 'Filecrypt', 'Gofile',
 'AkiraBox', 'Fpkg - VikingFile', 'Fpkg Backport - DataNodes', 'Fpkg Standard - Filed', '4.xx BackPort - DataNodes', '7.xx BackPort - Vault', 'Standard 9.xx - BuzzHeavier', 'Dump - AkiraBox', 'Fix - DataNodes', 'DLC - Filek',
 'Viki', 'Archive', 'Akirabox - pt.7', 'Akirabox - 4.xx+ - pt.5', 'Viki - DLC', 'Viki (+Arabic Lang)', 'Viki - Backport - 4.xx+', '1fichier (Backport 4.xx)', 'Archive (Game + DLC)', 'Archive - 5xx (Game + DLC)', 'Archive - FW 10.xx - 10.xx+', '1fichier- 5.xx Backport', 'Data - DLC by Psykzhack - DLC', 'FULL GAME UNLOCK (DUPLEX \u2013 PKG) - DLC',
 'BASE GAME - akirabox.com', 'BASE GAME + DLC - akirabox.to', 'FPKG BACKPORT - akirabox.to', 'Game - vik1ngfile.site', 'DLC - fuckingfast.net',
]
missing=[w for w in want if w not in names]
print('missing', missing)
cols=['region','fw','backport_target','kind','format','version','part','host','extra']
print('| # | Source | Link name (exact) | region | fw | BP target | kind | format | version | part | host | other |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|')
i=0
for w in want:
    if w not in names: continue
    i+=1
    src,url=names[w]; f=parse_link(w,url)
    part = str(f['part']) if f['part'] is not None else ('? (cut)' if f['part_unknown'] else '')
    kind = f['kind'] + ('*' if f['kind_inferred'] else '') + (' all' if f['all_dlc'] else '')
    extra=[]
    if f['backport'] and not f['backport_target']: extra.append('backport (target ?)')
    if f['build']: extra.append('build=standard')
    if f['packager']: extra.append('@'+f['packager'])
    if f['tags']: extra.append(','.join(f['tags']))
    if f['lang_to']: extra.append('langs '+f['lang_from']+'->'+f['lang_to'])
    if f['release']: extra.append('release '+str(f['release']))
    if f['fw_plus']: extra.append('fw+')
    if f['label']: extra.append('label "'+f['label']+'"')
    if f['size_text']: extra.append('size '+f['size_text'])
    if f['title_id']: extra.append(f['title_id'])
    if f['truncated']: extra.append('truncated')
    if f['host_from']=='name': extra.append('host from name')
    nm=w
    print(f"| {i} | {src.split()[0]} | `{nm}` | {f['region'] or ''} | {f['fw'] or ''} | {f['backport_target'] or ''} | {kind} | {f['format'] or ''} | {f['version'] or ''} | {part} | {f['host'] or ''} | {'; '.join(extra)} |")
