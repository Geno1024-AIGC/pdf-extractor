#!/usr/bin/env python3
"""Generate a small test PDF exercising the pdf-extractor parser:
   - an uncompressed dict
   - a FlateDecode over a small original embedded stream is simulated:
     we embed a fake image data stream that zlib.decompress will reject,
     and a plain-text /Length-bearing stream for validation.
   The goal is to produce objects the tool can list and extract.
"""
import zlib
import sys

def make_flate(data: bytes) -> bytes:
    return zlib.compress(data)

orig = b"hello pdf-extractor\nsecond line of content\n"
flate = make_flate(orig)

length_direct = len(flate)

objs = []

# object 1: simple dict
objs.append(b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")

# object 2: dict with kids empty
objs.append(b"2 0 obj\n<< /Type /Pages /Count 0 >>\nendobj\n")

# object 3: a stream with direct Length + FlateDecode
objs.append(
    b"3 0 obj\n"
    b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % length_direct
    + flate
    + b"\nendstream\nendobj\n"
)

# object 4: a number used as an indirect Length target (= length of flate5)
# object 5: stream whose /Length is an indirect reference -> 4 0 R
flate5 = make_flate(b"indirect length works")
len_bytes = ("%d" % len(flate5)).encode()
objs.append(b"4 0 obj\n" + len_bytes + b"\nendobj\n")
objs.append(
    b"5 0 obj\n"
    b"<< /Length 4 0 R /Filter /FlateDecode >>\nstream\n" % ()
    + flate5
    + b"\nendstream\nendobj\n"
)

# object 6: font stream (uncompressed), no filter
font = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
objs.append(
    b"6 0 obj\n"
    b"<< /Type /Font /Subtype /Type1 /Length %d >>\nstream\n" % len(font)
    + font
    + b"\nendstream\nendobj\n"
)

body = b"".join(objs)

xref_offset = len(b"%PDF-1.4\n") + len(body) + 0
pdf = b"%PDF-1.4\n" + body

# compute offsets
offsets = {}
header_len = len(b"%PDF-1.4\n")
pos = header_len
for i, obj in enumerate(objs, start=1):
    offsets[i] = pos
    pos += len(obj)
xref_offset = pos

pdf += b"xref\n"
pdf += b"0 %d\n" % (len(objs) + 1)
pdf += b"0000000000 65535 f \n"
for i in range(1, len(objs) + 1):
    pdf += b"%010d 00000 n \n" % offsets[i]
pdf += b"trailer\n<< /Size %d /Root 1 0 R >>\n" % (len(objs) + 1)
pdf += b"startxref\n%d\n%%%%EOF\n" % xref_offset

out = sys.argv[1] if len(sys.argv) > 1 else "test.pdf"
with open(out, "wb") as f:
    f.write(pdf)
print("wrote", out, len(pdf), "bytes")