# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
import sys
pats = {
 '0x62 table dl (40 62 04 01 80 06 00 03)': bytes.fromhex('40620401800600 03'.replace(' ','')),
 '0x1f (40 1f 86 30)':                      bytes.fromhex('401f8630'),
 '0xe3 (c0 e3 03 00 00 fc 00 01)':          bytes.fromhex('c0e30300 00fc0001'.replace(' ','')),
 '0x16 (40 16 09 00 ff 01 00 00)':          bytes.fromhex('4016090 0ff010000'.replace(' ','')),
 '0x34 ident (c0 34 00 00 00 00 37 00)':    bytes.fromhex('c034000000003700'),
 '0x30 setup (40 30 00 00 00 00 78 00)':    bytes.fromhex('403000000000 7800'.replace(' ','')),
 '0x00 status (c0 00 00 00 00 00 20 00)':   bytes.fromhex('c000000000002000'),
}
for path in sys.argv[1:]:
    data = open(path,'rb').read()
    print('==', path.split('/')[-1], len(data))
    for name, p in pats.items():
        hits = []
        i = data.find(p)
        while i >= 0 and len(hits) < 6:
            hits.append(hex(i)); i = data.find(p, i+1)
        print('   %-42s %s' % (name, hits or 'not found'))
