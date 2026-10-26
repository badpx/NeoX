#!/usr/bin/env python3
"""Generate a tiny original polygon font using only Python's standard library.
Geometry is intentionally simple enough for an independent analytic oracle.
No third-party font data, rasterizer or distance-field code is used here.
"""
from pathlib import Path
import struct
U16=lambda x:struct.pack('>H',x)
I16=lambda x:struct.pack('>h',x)
U32=lambda x:struct.pack('>I',x)
# Clockwise outer contours and counterclockwise holes, in font units.
shapes=[[],[[(100,0),(500,800),(900,0)]],
        [[(100,0),(100,800),(900,800),(900,0)],[(300,200),(700,200),(700,600),(300,600)]],
        [[(475,0),(475,800),(525,800),(525,0)]]]
glyf=b'';offsets=[0]
for contours in shapes:
 if contours:
  pts=[p for c in contours for p in c];xs,ys=zip(*pts)
  g=I16(len(contours))+b''.join(I16(x) for x in (min(xs),min(ys),max(xs),max(ys)))
  n=0
  for c in contours:n+=len(c);g+=U16(n-1)
  g+=U16(0)+bytes([1]*len(pts))
  for axis in (0,1):
   last=0
   for p in pts:g+=I16(p[axis]-last);last=p[axis]
  glyf+=g+b'\0'*((-len(g))%4)
 offsets.append(len(glyf))
head=bytearray(54);head[:4]=U32(0x10000);head[12:16]=U32(0x5f0f3cf5);head[18:20]=U16(1000);head[50:52]=I16(1)
hhea=bytearray(36);hhea[:4]=U32(0x10000);hhea[4:8]=I16(800)+I16(-200);hhea[34:36]=U16(4)
maxp=U32(0x10000)+U16(4)+bytes(26)
codes=bytearray(256)
for i,c in enumerate('THI',1):codes[ord(c)]=i
cmap=U16(0)+U16(1)+U16(3)+U16(1)+U32(12)+U16(0)+U16(262)+U16(0)+codes
tables={'head':bytes(head),'hhea':bytes(hhea),'maxp':maxp,'hmtx':(U16(1000)+I16(100))*4,
        'loca':b''.join(U32(n) for n in offsets),'glyf':glyf,'cmap':cmap}
def checksum(b):
 b+=b'\0'*((-len(b))%4);return sum(struct.unpack('>'+str(len(b)//4)+'I',b))&0xffffffff
header=U32(0x10000)+U16(7)+U16(64)+U16(2)+U16(48)
records=b'';body=b'';offset=12+16*7
for tag,data in sorted(tables.items()):
 records+=tag.encode()+U32(checksum(data))+U32(offset)+U32(len(data))
 if tag=='head':head_offset=offset
 padded=data+b'\0'*((-len(data))%4);body+=padded;offset+=len(padded)
font=bytearray(header+records+body);font[head_offset+8:head_offset+12]=U32((0xb1b0afba-checksum(bytes(font)))&0xffffffff)
Path(__file__).with_name('quality-polygons.ttf').write_bytes(font)
