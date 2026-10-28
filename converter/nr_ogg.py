"""Ogg Vorbis helpers for nr_audio: read ww2ogg output, recompute granule positions and
repaginate (ww2ogg writes one packet per page with granule 0, which breaks seeking)."""
import struct

def _crc_table():
    t = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        t.append(r & 0xFFFFFFFF)
    return t
_CRC = _crc_table()
_REV = bytes(int('{:08b}'.format(i)[::-1], 2) for i in range(256))
def ogg_crc_slow(data):
    c = 0
    for b in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ _CRC[((c >> 24) & 0xFF) ^ b]
    return c
def ogg_crc(data):
    """Ogg's CRC (poly 0x04C11DB7, MSB first, init 0, no final xor) via zlib's reflected CRC-32."""
    import zlib
    raw = zlib.crc32(bytes(data).translate(_REV), 0xFFFFFFFF) ^ 0xFFFFFFFF
    return int('{:032b}'.format(raw)[::-1], 2)

def read_packets(d):
    """Return list of packets (bytes) from an ogg byte string (single logical stream)."""
    pk = []; cur = b''; p = 0
    while p < len(d):
        if d[p:p + 4] != b'OggS': raise ValueError('bad ogg page at %d' % p)
        ns = d[p + 26]; segs = d[p + 27:p + 27 + ns]; q = p + 27 + ns
        for s in segs:
            cur += d[q:q + s]; q += s
            if s < 255: pk.append(cur); cur = b''
        p = q
    if cur: pk.append(cur)
    return pk

class _RevBits:
    """Read the bitstream of `buf` backwards (last bit first)."""
    def __init__(self, buf):
        self.b = buf[::-1]; self.pos = 0
    def left(self): return len(self.b) * 8 - self.pos
    def get(self, n):
        v = 0
        for _ in range(n):
            byte = self.b[self.pos >> 3]; bit = (byte >> (7 - (self.pos & 7))) & 1
            v = (v << 1) | bit; self.pos += 1
        return v

def mode_blockflags(setup):
    """Mode block flags from a Vorbis setup header, read from the end (same trick as ffmpeg)."""
    g = _RevBits(setup)
    framing = 0
    while g.left() > 97:
        if g.get(1): framing = g.pos; break
    if not framing: raise ValueError('no framing bit')
    mode_count = 0; last = 0
    while g.left() >= 97:
        if g.get(8) > 63 or g.get(16) or g.get(16): break
        g.get(1); mode_count += 1
        if mode_count > 64: break
        save = g.pos
        if g.get(6) + 1 == mode_count: last = mode_count
        g.pos = save
    if not last: raise ValueError('no mode header')
    g = _RevBits(setup); g.pos = framing
    flags = [0] * last
    for i in range(last - 1, -1, -1):
        g.get(40); flags[i] = g.get(1)
    return flags

def packet_samples(packets):
    """Samples produced by each packet (0 for headers and the first audio packet)."""
    ident = packets[0]
    bs0 = 1 << (ident[28] & 15); bs1 = 1 << (ident[28] >> 4)
    flags = mode_blockflags(packets[2])
    mbits = max(1, (len(flags) - 1).bit_length()) if len(flags) > 1 else 0
    out = [0, 0, 0]; prev = None
    for p in packets[3:]:
        if not p or (p[0] & 1): out.append(0); continue
        mode = (p[0] >> 1) & ((1 << mbits) - 1) if mbits else 0
        bs = bs1 if flags[mode] else bs0
        out.append(0 if prev is None else (prev + bs) // 4)
        prev = bs
    return out

def write_ogg(packets, total_samples=None, serial=0x4E52, page_target=4096):
    """Repaginate packets with correct granules. Headers: id on its own page, comment+setup next."""
    ns = packet_samples(packets)
    out = bytearray(); seq = 0
    def page(pkts, gran, flags):
        nonlocal seq
        lace = bytearray(); body = bytearray()
        for i, p in enumerate(pkts):
            n = len(p)
            while n >= 255: lace.append(255); n -= 255
            lace.append(n); body += p
        assert len(lace) <= 255
        hdr = bytearray(b'OggS' + bytes([0, flags]) + struct.pack('<qIII', gran, serial, seq, 0) + bytes([len(lace)]) + lace)
        pg = hdr + body
        struct.pack_into('<I', pg, 22, ogg_crc(pg))
        out.extend(pg); seq += 1
    page([packets[0]], 0, 2)
    page([packets[1], packets[2]], 0, 0)
    gran = 0; cur = []; cursz = 0; curlace = 0
    audio = list(range(3, len(packets)))
    for k, i in enumerate(audio):
        p = packets[i]; lace = len(p) // 255 + 1
        if cur and (cursz + len(p) > page_target or curlace + lace > 255):
            page([packets[j] for j in cur], gran, 0); cur = []; cursz = 0; curlace = 0
        cur.append(i); cursz += len(p); curlace += lace; gran += ns[i]
    if cur:
        g = gran if total_samples is None else min(gran, total_samples)
        page([packets[j] for j in cur], g, 4)
    return bytes(out), gran
