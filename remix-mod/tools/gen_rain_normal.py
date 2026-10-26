import numpy as np, sys
from PIL import Image
rng=np.random.default_rng(7)
FW,FH,COLS,ROWS=768,384,4,2
N=COLS*ROWS
def dome(h,cx,cy,rx,ry,amp=1.0):
    x0,x1=int(max(cx-rx-1,0)),int(min(cx+rx+2,FW)); y0,y1=int(max(cy-ry-1,0)),int(min(cy+ry+2,FH))
    if x0>=x1 or y0>=y1: return
    yy,xx=np.mgrid[y0:y1,x0:x1]
    d=((xx-cx)/rx)**2+((yy-cy)/ry)**2
    v=np.sqrt(np.clip(1-d,0,1))*amp
    h[y0:y1,x0:x1]=np.maximum(h[y0:y1,x0:x1],v)
static=[(rng.uniform(0,FW),rng.uniform(0,FH),r) for r in rng.uniform(1.2,4.5,600)]
movers=[(rng.uniform(0,FW),rng.uniform(0,FH),rng.uniform(4,7),rng.uniform(FH*0.3,FH*0.9)) for _ in range(25)]
frames=[]
for f in range(N):
    h=np.zeros((FH,FW),np.float32)
    for x,y,r in static: dome(h,x,y,r,r*1.1)
    t=f/N
    for x,y,r,speed in movers:
        cy=(y+t*speed)%FH
        dome(h,x,cy,r,r*1.4)
        for k in range(1,12):  # thin wet trail above the drop
            dome(h,x+np.sin(k*0.7)*0.8,(cy-k*r*0.9)%FH,r*0.35,r*0.6,0.5)
    gy,gx=np.gradient(h*6.0)
    nx,ny,nz=-gx,-gy,np.ones_like(h)
    l=np.sqrt(nx*nx+ny*ny+nz*nz); nx,ny,nz=nx/l,ny/l,nz/l
    rgb=np.dstack([(nx*0.5+0.5),(ny*0.5+0.5),(nz*0.5+0.5)])
    frames.append((rgb*255+0.5).astype(np.uint8))
sheet=np.zeros((FH*ROWS,FW*COLS,3),np.uint8)
for i,fr in enumerate(frames):
    r,c=divmod(i,COLS); sheet[r*FH:(r+1)*FH,c*FW:(c+1)*FW]=fr
img=Image.fromarray(sheet,'RGB').convert('RGBA')
img.save(sys.argv[1])
img.convert('RGB').resize((1536,384)).save(sys.argv[2])
print(img.size)
