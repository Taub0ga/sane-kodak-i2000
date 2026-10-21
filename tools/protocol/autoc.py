from usbmon import read
import statistics
us = read("/home/tauboga/Projekte/Kodak i2600/captures/pcapng/c-duplex-lineart-200.pcapng", dev=6)
tr = [u for u in us if u.type=='C' and u.xfer==3 and u.ep==0x82 and u.status==0 and u.len_cap>=4000]
# pick a mid-page chunk with high variance (real content)
best = max(tr[300:500], key=lambda u: statistics.pvariance(u.data[:4000]))
d = best.data[:4032]
print('chunk len', len(d), 'mean %.1f var %.0f' % (statistics.mean(d), statistics.pvariance(d)))
n = len(d); mu = statistics.mean(d)
x = [v - mu for v in d]
den = sum(v*v for v in x)
res = []
for lag in range(8, 2000):
    s = sum(x[i]*x[i+lag] for i in range(n-lag))
    res.append((s/ (den * (n-lag)/n), lag))
res.sort(reverse=True)
print('top 15 autocorrelation lags:')
for r, lag in res[:15]:
    print('   lag %5d  r=%.3f' % (lag, r))
