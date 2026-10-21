# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Name the bRequest 0xe0 accesses of a capture (camera device / AFE device).

0xe0 is DeviceSettings::Read/WriteAfeDevice and Read/WriteCameraDevice:
    wValue = register address inside the selected page
    wIndex = device index (camera 0 = front/master, 1 = rear/slave)
Register 0x1f of the camera device is the page select (SetCameraPageValue),
so a camera access is always the triple  write 0x1f / write 0x1f / read 0x1f
followed by the access to the target register of that page.

Usage: python3 camregs.py captures/pcapng/b-simplex-lineart-200.pcapng
"""
import sys
from ctrl import transfers
from usbmon import hx

PAGE_REG = 0x1f

# What the driver is known to use (devicemanager.so call sites); see INITIALISIERUNG.md
KNOWN = {
    (0, 0x02): 'camera control (SetAfeStandby: 0x20; SetCameraResolution: 0x20, 0x28, 0x08, 0x19)',
    (0, 0x00): 'page 0 register block (32 B from s_dmcamera*)',
    (1, 0x00): 'AFE gains, 1 byte per channel (Read/WriteAllAFEGains, WriteAFEGain)',
    (2, 0x05): 'SetAfeStandby',
    (3, 0x00): 'SetAfeStandby', (3, 0x01): 'SetAfeStandby', (3, 0x02): 'SetAfeStandby',
    (3, 0x03): 'SetAfeStandby', (3, 0x04): 'SetAfeStandby',
    (6, 0x01): 'SetCameraResolution (0x22)',
}

def main(caps):
    for cap in caps:
        print('=== %s' % cap.split('/')[-1])
        page = {}
        for t in transfers(cap):
            if (t['bmr'] & 0x60) != 0x40 or t['breq'] != 0xe0:
                continue
            d = 'IN ' if t['bmr'] & 0x80 else 'OUT'
            cam, reg = t['widx'], t['wval']
            if reg == PAGE_REG:
                if d == 'OUT':
                    page[cam] = t['data'][0] if t['data'] else None
                    print('%8.3f %s cam=%d  page select   <- %s'
                          % (t['ts'], d, cam, hx(t['data'])))
                else:
                    print('%8.3f %s cam=%d  page select   -> %s  (read back)'
                          % (t['ts'], d, cam, hx(t['data'])))
                continue
            p = page.get(cam)
            note = KNOWN.get((p, reg), '')
            print('%8.3f %s cam=%d  page %-4s reg 0x%02x [%2d]  %-34s %s'
                  % (t['ts'], d, cam, ('%d' % p) if p is not None else '?', reg,
                     t['wlen'], hx(t['data']), note))

if __name__ == '__main__':
    main(sys.argv[1:])
