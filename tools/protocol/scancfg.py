# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Decode the 120-byte SDmScanConfig block of bRequest 0x30 from a capture.

Field offsets come from the one-instruction setters of
DeviceManager::CScannerConfiguration (devicemanager.so): each Set*() writes a
single field at a fixed offset of the object; the SDmScanConfig part of the
object starts at +8 (the DeviceSettings reference occupies +0..+7).

Usage: python3 scancfg.py captures/pcapng/b-simplex-lineart-200.pcapng [...]
"""
import sys, struct
from ctrl import transfers

# (offset, size, name, enum)                       size 1/2/4, little endian
FEEDTYPE   = {0: 'Normal', 1: 'StopIfEmpty'}
FEEDSTYLE  = {0: 'None', 1: 'Normal'}
CAPMODE    = {0: 'Disabled', 1: 'Normal', 2: 'shutter'}
SPEEDID    = {0: 'Stopped', 2: '600 dpi', 4: '300 dpi', 5: '200 dpi'}
IMAGEDATA  = {0: 'Color', 1: 'Gray'}
MFRESPONSE = {0: 'Notify only', 1: 'Stop and leave paper', 2: 'Stop and clear'}
ENDIS      = {0: 'Disabled', 1: 'Enabled'}
CAPRATE    = {0x0f: 'All lines'}

FIELDS = [
    (0x00, 1, 'FeedType',                             FEEDTYPE),
    (0x01, 1, 'FeedStyle',                            FEEDSTYLE),
    (0x02, 2, 'PageCount',                            None),
    (0x04, 2, 'TransportTimeout',                     None),
    (0x06, 2, 'MaxPageLength',                        None),
    (0x08, 2, 'MultifeedLength',                      None),
    (0x0a, 2, 'GapLength',                            None),
    (0x0d, 2, 'FrontCaptureLEOffset',                 None),
    (0x0f, 2, 'RearCaptureLEOffset',                  None),
    (0x11, 1, 'TraySensorMode',                       ENDIS),
    (0x12, 2, 'FrontCaptureTEOffset',                 None),
    (0x14, 2, 'RearCaptureTEOffset',                  None),
    (0x16, 2, 'PrinterOffset',                        None),
    (0x18, 1, 'PauseEnable',                          ENDIS),
    (0x19, 4, 'MaxPrintCount',                        None),
    (0x1d, 2, 'HoleSize',                             None),
    (0x20, 1, 'SpeedId',                              SPEEDID),
    (0x21, 1, 'DynamicSpeedId',                       SPEEDID),
    (0x22, 1, 'ThrottleTimeout',                      None),
    (0x23, 1, 'RampId',                               None),
    (0x24, 1, 'ImageDataId',                          IMAGEDATA),
    (0x25, 1, 'Stop',                                 None),
    (0x26, 2, 'NotificationOffset',                   None),
    (0x28, 2, 'MagnificationTargetFixedLength',       None),
    (0x2a, 2, 'MagnificationTargetCalibrationLength', None),
    (0x30, 1, 'CaptureMode',                          CAPMODE),
    (0x31, 1, 'CaptureStyle (front and rear)',        None),
    (0x32, 2, 'FrontOverscanLength',                  None),
    (0x34, 2, 'RearOverscanLength',                   None),
    (0x36, 1, 'FifoEnable',                           None),
    (0x37, 1, 'CaptureRate',                          CAPRATE),
    (0x38, 2, 'MultifeedResponse',                    MFRESPONSE),
    (0x40, 1, 'PatchToggleMode',                      None),
    (0x48, 2, 'FeederOnDistance',                     None),
]
KNOWN = set()
for o, s, _n, _e in FIELDS:
    KNOWN.update(range(o, o + s))

def get(d, off, size):
    if size == 1: return d[off]
    if size == 2: return struct.unpack_from('<H', d, off)[0]
    return struct.unpack_from('<I', d, off)[0]

def dump(d):
    for off, size, name, enum in FIELDS:
        v = get(d, off, size)
        s = '  0x%02x  %-38s %-7d  0x%0*x' % (off, name, v, size * 2, v)
        if enum is not None:
            s += '   %s' % enum.get(v, '?')
        if name == 'FifoEnable':
            s += '   bit0=front bit1=rear (bits 4/5 additionally set in the shutter pass)'
        print(s)
    rest = [i for i in range(len(d)) if i not in KNOWN and d[i]]
    print('  bytes outside the known fields that are non-zero: %s'
          % (', '.join('0x%02x=0x%02x' % (i, d[i]) for i in rest) or 'none'))

if __name__ == '__main__':
    for cap in sys.argv[1:]:
        print('=== %s' % cap.split('/')[-1])
        n = 0
        for t in transfers(cap):
            if (t['bmr'] & 0x60) != 0x40 or t['breq'] != 0x30 or t['wlen'] != 120:
                continue
            n += 1
            print('--- 0x30 #%d  t=%.3f' % (n, t['ts']))
            d = t['data']
            for i in range(0, len(d), 16):
                print('      %02x  %s' % (i, ' '.join('%02x' % c for c in d[i:i+16])))
            dump(d)
