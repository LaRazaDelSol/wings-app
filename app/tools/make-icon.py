# Wings TV app - generates sce_sys/icon0.png (flat colour, W mark, WINGS wordmark).
# SPDX-License-Identifier: GPL-3.0-or-later
# usage: python3 tools/make-icon.py sce_sys/icon0.png   (needs numpy)
import numpy as np, zlib, struct, sys
N=512; SS=3
def dist_seg(px,py,a,b):
    dx,dy=b[0]-a[0],b[1]-a[1]; l2=dx*dx+dy*dy
    t=np.clip(((px-a[0])*dx+(py-a[1])*dy)/l2,0,1) if l2>0 else 0
    return np.hypot(a[0]+t*dx-px,a[1]+t*dy-py)
def poly(px,py,pts,half):
    d=np.full(px.shape,1e9)
    for a,b in zip(pts,pts[1:]): d=np.minimum(d,dist_seg(px,py,a,b))
    return d<=half
def circ(px,py,c,r,half):
    return np.abs(np.hypot(px-c[0],py-c[1])-r)<=half
# supersampled grid
M=N*SS
ys,xs=np.mgrid[0:M,0:M]; px=(xs+0.5)/SS; py=(ys+0.5)/SS
mask=np.zeros((M,M),bool)
# the mark
W=[(70,60),(170,282),(256,124),(342,282),(442,60)]
mask|=poly(px,py,[(x,y+20) for x,y in W],24)
# wordmark WINGS in capitals, one box per letter
def letter(ch,x0,y0,w,h,half):
    def P(u,v): return (x0+u*w, y0+v*h)
    pts={'W':[(0,0),(.25,1),(.5,.35),(.75,1),(1,0)],
         'I':[(.5,0),(.5,1)],
         'N':[(0,1),(0,0),(1,1),(1,0)],
         'G':[(1,.2),(.7,0),(.3,0),(0,.2),(0,.8),(.3,1),(.7,1),(1,.8),(1,.52),(.55,.52)],
         'S':[(1,.18),(.7,0),(.3,0),(0,.2),(.1,.42),(.5,.5),(.9,.58),(1,.8),(.7,1),(.3,1),(0,.82)]}[ch]
    return poly(px,py,[P(*p) for p in pts],half)
widths={'W':96,'I':10,'N':60,'G':62,'S':58}
gap=30
total=sum(widths[c] for c in 'WINGS')+gap*4
x=(N-total)/2
for ch in 'WINGS':
    w=widths[ch]
    mask|=letter(ch,x,372,w,70,6.5)
    x+=w+gap
cov=mask.reshape(N,SS,N,SS).mean(axis=(1,3))
bg=np.array([0x14,0x32,0x5e],float); fg=np.array([255,255,255],float)
# subtle vertical gradient on the flat colour
g=np.linspace(1.12,0.88,N)[:,None,None]
img=(bg*g)*(1-cov[...,None])+fg*cov[...,None]
img=np.clip(img,0,255).astype(np.uint8)
raw=b''.join(b'\x00'+img[y].tobytes() for y in range(N))
def chunk(t,d):
    c=struct.pack('>I',len(d))+t+d
    return c+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',N,N,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b'')
open(sys.argv[1],'wb').write(png)
