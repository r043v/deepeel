import sys
a=open(sys.argv[1],'rb'); b=open(sys.argv[2],'rb'); skip=int(sys.argv[3]) if len(sys.argv)>3 else 0
Z={'stack fc00-ffff':(0xfc00,0x10000),'vars  f000-fbff':(0xf000,0xfc00),'objs  d000-efff':(0xd000,0xf000),'level 0000-cfff':(0,0xd000)}
first={}; tot={k:0 for k in Z}; n=0; rows=[]
while True:
    x=a.read(0x10000); y=b.read(0x10000)
    if len(x)<0x10000 or len(y)<0x10000: break
    n+=1
    if x==y: continue
    d={k:sum(1 for i in range(lo,hi) if x[i]!=y[i]) for k,(lo,hi) in Z.items()}
    for k,v in d.items():
        if v: first.setdefault(k,n); tot[k]+=1
    if n%300==0: rows.append((n,d))
print("frames",n)
for k in Z: print(k,"first diff frame",first.get(k),"frames differing",tot[k])
for n,d in rows: print(n,d)
