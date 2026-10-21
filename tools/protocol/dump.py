# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
import sys
from ctrl import transfers
from usbmon import hx
for path in sys.argv[1:]:
    name = path.split('/')[-1].replace('.pcapng','')
    with open('ctrl-%s.txt' % name, 'w') as fh:
        for t in transfers(path):
            if (t['bmr'] & 0x60) != 0x40:
                continue   # vendor only
            d = t['data']
            fh.write('%s %-18s%s\n' % (
                'OUT' if not (t['bmr'] & 0x80) else 'IN ',
                'req=%02x val=%04x idx=%04x len=%d' % (t['breq'], t['wval'], t['widx'], t['wlen']),
                (' | ' + hx(d)) if d else ''))
    print('wrote ctrl-%s.txt' % name)
