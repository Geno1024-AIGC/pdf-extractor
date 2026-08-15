#!/usr/bin/env python3
"""Generate a richer test PDF for pdf-extractor:
   - a FlateDecode text stream (extract -> txt)
   - an ASCII85Decode stream (extract -> txt)
   - an uncompressed font stream (no filter)
   - a DCTDecode "image" stream (fake JPEG header so extension -> jpg)
   - an indirect /Length stream
"""
import base64
import sys
import zlib


def a85(data: bytes) -> bytes:
    # Adobe ASCII85: '<~' + encoded + '~>' ; a85encode already handles the
    # final partial group, so no padding to a multiple of 5 is added.
    return b"<~" + base64.a85encode(data, adobe=False) + b"~>"


parts = []
parts.append(b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")
parts.append(b"2 0 obj\n<< /Type /Pages /Count 0 >>\nendobj\n")

# 3: FlateDecode text
text1 = b"hello pdf-extractor\nflate text stream line two\n"
parts.append(
    b"3 0 obj\n<< /Length %d /Filter /FlateDecode >>\nstream\n"
    % len(zlib.compress(text1))
    + zlib.compress(text1)
    + b"\nendstream\nendobj\n"
)

# 4: ASCII85 text
text2 = b"ascii85 encoded content, some more text here\n"
parts.append(
    b"4 0 obj\n<< /Length %d /Filter /ASCII85Decode >>\nstream\n"
    % len(a85(text2))
    + a85(text2)
    + b"\nendstream\nendobj\n"
)

# 5: font stream (uncompressed)
font = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
parts.append(
    b"5 0 obj\n<< /Type /Font /Subtype /Type1 /Length %d >>\nstream\n" % len(font)
    + font
    + b"\nendstream\nendobj\n"
)

# 6: fake JPEG "image" (DCTDecode); content is not a real JPEG but the header
# signature makes guessExtension() return .jpg
jpg = b"\xff\xd8\xff\xe0" + b"\x00" * 40 + b"\xff\xd9"
parts.append(
    b"6 0 obj\n<< /Type /XObject /Subtype /Image /Width 4 /Height 4 "
    b"/ColorSpace /DeviceRGB /Filter /DCTDecode /Length %d >>\nstream\n" % len(jpg)
    + jpg
    + b"\nendstream\nendobj\n"
)

# 7: indirect length target, 8: FlateDecode stream with indirect /Length
text3 = b"indirect length is resolved correctly\n"
flate3 = zlib.compress(text3)
parts.append(b"7 0 obj\n%d\nendobj\n" % len(flate3))
parts.append(
    b"8 0 obj\n<< /Length 7 0 R /Filter /FlateDecode >>\nstream\n"
    + flate3
    + b"\nendstream\nendobj\n"
)

body = b"".join(parts)
header = b"%PDF-1.4\n"
offsets = {}
pos = len(header)
for i, p in enumerate(parts, start=1):
    offsets[i] = pos
    pos += len(p)

pdf = header + body
pdf += b"xref\n0 %d\n" % (len(parts) + 1)
pdf += b"0000000000 65535 f \n"
for i in range(1, len(parts) + 1):
    pdf += b"%010d 00000 n \n" % offsets[i]
pdf += b"trailer\n<< /Size %d /Root 1 0 R >>\n" % (len(parts) + 1)
pdf += b"startxref\n%d\n%%%%EOF\n" % pos

out = sys.argv[1] if len(sys.argv) > 1 else "rich.pdf"
with open(out, "wb") as f:
    f.write(pdf)
print("wrote", out, len(pdf), "bytes")