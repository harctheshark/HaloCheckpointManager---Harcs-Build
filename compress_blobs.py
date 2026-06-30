# Index (dedupe verts) + zlib-compress RINST001 soup blobs -> RINST002. Run after reach_extract_all.ps1.
# Usage: python compress_blobs.py <in_root> <out_root>
import struct, zlib, os, sys, numpy as np

def compress(inp, outp):
    with open(inp,'rb') as f: d=f.read()
    if d[:8]!=b'RINST001': return None
    tc=struct.unpack_from('<i',d,8)[0]
    if tc<=0:
        # empty blob -> still write a valid empty RINST002
        payload=struct.pack('<ii',0,0)
    else:
        arr=np.frombuffer(d,dtype=np.float32,count=tc*9,offset=12).reshape(-1,3)  # tc*3 verts
        uniq,inv=np.unique(arr,axis=0,return_inverse=True)
        inv=inv.reshape(-1).astype(np.int32)
        payload=struct.pack('<ii',int(uniq.shape[0]),tc)+uniq.astype('<f4').tobytes()+inv.astype('<i4').tobytes()
    comp=zlib.compress(payload,9)
    os.makedirs(os.path.dirname(outp),exist_ok=True)
    with open(outp,'wb') as f:
        f.write(b'RINST002'); f.write(struct.pack('<i',len(payload))); f.write(comp)
    return (len(d), os.path.getsize(outp), tc)

inroot=sys.argv[1]; outroot=sys.argv[2]
files=[]
for dp,_,fns in os.walk(inroot):
    for fn in fns:
        if fn.endswith('.rinst'): files.append(os.path.join(dp,fn))
files.sort()
tin=tout=0; n=0
for i,f in enumerate(files):
    rel=os.path.relpath(f,inroot)
    out=os.path.join(outroot,rel)
    r=compress(f,out)
    if r:
        tin+=r[0]; tout+=r[1]; n+=1
        if i%20==0 or r[0]>5_000_000:
            print(f"[{i+1}/{len(files)}] {rel}  {r[0]//1024}KB -> {r[1]//1024}KB ({100*r[1]/max(1,r[0]):.0f}%) tris={r[2]}")
print(f"DONE {n} blobs: {tin/1024/1024:.0f}MB -> {tout/1024/1024:.0f}MB ({100*tout/max(1,tin):.0f}%)")
