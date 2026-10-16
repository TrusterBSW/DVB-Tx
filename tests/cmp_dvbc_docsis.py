#!/usr/bin/env python3
"""Compare the dvbtx DVB-C symbols with the J.83A encoder of mini-cmts (validated with a cable modem)."""
import sys
import numpy as np
sys.path.insert(0, sys.argv[1])            # path of the Docsis project (read only)
from cmts import j83a
ts, mine, qam, npk = sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
pk = np.fromfile(ts, np.uint8, count=npk * 188).reshape(-1, 188)
enc = j83a.J83AEncoder(qam)
enc.use_c = False
ref = enc.encode_py(pk)
x = np.fromfile(mine, np.complex64)
n = min(x.size, ref.size)
x, ref = x[:n], ref[:n]
bad = np.nonzero(np.abs(x - ref) > 1e-4)[0]
print(f"{qam}-QAM: {ref.size} symbols compared, {bad.size} differ" + (f", first at {bad[:5]}" if bad.size else ""))
