"""cam_dis.py ADDR [N]: disassemble guest code at ADDR (hex), resolving inter-TOC stubs (std r2;addis;addi;b) and printing the final target."""
import sys,struct
from capstone import *
d=open('elf/EBOOT.ELF','rb').read()
md=Cs(CS_ARCH_PPC,CS_MODE_64|CS_MODE_BIG_ENDIAN)
def rd(a,n): return d[a-0x10000:a-0x10000+n] if a<0x830000+0x10000 else None
def resolve(a):
    w=struct.unpack('>4I',d[a-0x10000:a-0x10000+16])
    if w[0]==0xF8410028 and (w[3]>>26)==18:
        li=w[3]&0x3FFFFFC
        if li&0x2000000: li-=0x4000000
        return a+12+li
    return None
a=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 30
t=resolve(a)
while t: print("stub %x -> %x"%(a,t)); a=t; t=resolve(a)
for i in md.disasm(d[a-0x10000:a-0x10000+n*4],a): print("%x: %s %s"%(i.address,i.mnemonic,i.op_str))
