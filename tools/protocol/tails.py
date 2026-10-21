from usbmon import read, hx
names = ['b-simplex-lineart-200','c-duplex-lineart-200','e-simplex-lineart-300','f-simplex-gray-200']
base = "/home/tauboga/Projekte/Kodak i2600/captures/pcapng/%s.pcapng"
for n in names:
    us = read(base % n, dev=6)
    for ep in (0x82, 0x86):
        tr = [u for u in us if u.type=='C' and u.xfer==3 and u.ep==ep and u.status==0]
        if not tr: continue
        tot = 0; k = 0; b = 0
        for u in tr:
            tot += u.length; k += 1
            if u.length < 16384:
                b += 1
                full = u.len_cap >= u.length
                print('%s ep%02x burst%d: %d B, terminal xfer %d B, fully captured=%s'
                      % (n[:20], ep, b, tot, u.length, full))
                if full:
                    print('      tail 96 B: %s' % hx(u.data[-96:]))
                tot = 0; k = 0
