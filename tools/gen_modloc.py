# -*- coding: utf-8 -*-
# Regenerates the MODL fallback table in br2mod.cpp from the T dict in
# make_world.py. Latin languages are CP1252, RU is CP1251 (same as msglist).
import io, re

src = io.open('make_world.py', encoding='utf-8').read()
m = re.search(r'^T = \{.*?^\}', src, re.S | re.M)
d = {}
exec(m.group(0), {}, d)
T = d['T']

def cstr(s, enc):
    out = ''
    hexesc = False
    for c in s.encode(enc):
        if 32 <= c < 127 and c not in (34, 92):
            if hexesc and chr(c) in '0123456789abcdefABCDEF':
                out += '""'   # end the hex escape before a hex digit
            out += chr(c)
            hexesc = False
        else:
            out += '\\x%02X' % c
            hexesc = True
    return out

rows = []
for key, tr in T.items():
    rows.append(' {"%s", {"%s","%s","%s","%s","%s",0,"%s"}},' % (
        cstr(key, 'cp1252'),
        cstr(key, 'cp1252'), cstr(tr['FR'], 'cp1252'), cstr(tr['IT'], 'cp1252'),
        cstr(tr['ES'], 'cp1252'), cstr(tr['DE'], 'cp1252'), cstr(tr['RU'], 'cp1251')))

p = '../br2mod.cpp'
s = io.open(p, encoding='utf-8').read()
a = s.index('static const ModLoc MODL[] = {')
b = s.index('};', a) + 2
s = s[:a] + 'static const ModLoc MODL[] = {\n' + '\n'.join(rows) + '\n};' + s[b:]
io.open(p, 'w', encoding='utf-8', newline='\r\n').write(s)
print('rows:', len(rows))
