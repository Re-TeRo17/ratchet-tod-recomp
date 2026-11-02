"""cam_reg.py SITE_HEX...: for each 'lwz r4,off(r2); lwz r5,off2(r2)' registration pair at SITE (Lua name,func), resolve via each TOC; print the one whose name string is readable."""
import struct,sys
d=open('elf/EBOOT.ELF','rb').read()
segs=[(0x830000,0x840000,0x6d4e8),(0x8a0000,0x10000000,0x56b80),(0x900000,0x10060000,0x14ef00),(0,0x10000,0x82c3c8)]
def rd(v,n):
    for fo,s,sz in segs:
        if s<=v<s+sz: return d[fo+v-s:fo+v-s+n]
def u32(v): return struct.unpack('>I',rd(v,4))[0]
def s16(x): return x-65536 if x&0x8000 else x
for t in sys.argv[1:]:
    a=int(t,16)
    w1=u32(a); w2=u32(a+4)
    o1=s16(w1&0xffff); o2=s16(w2&0xffff)
    for toc in (0x88FF38,0x89FF20,0x8AFE5C):
        try:
            p=u32(toc+o1); f=u32(toc+o2)
            nm=rd(p,40).split(b'\0')[0]
            if len(nm)>2 and all(32<=c<127 for c in nm): print(hex(a),hex(toc),nm.decode(),'func/opd',hex(f),'code',hex(u32(f)))
        except Exception as e: pass
