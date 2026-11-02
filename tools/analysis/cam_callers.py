"""callers.py HEXADDR...: find bl/b callers of targets, including via inter-TOC stubs (one level). Prints caller addr, enclosing func."""
import struct,sys,json,bisect
d=open('elf/EBOOT.ELF','rb').read()
fs=json.load(open('out/analysis/funcs.json'))
tg=set(int(x,16) for x in sys.argv[1:])
end=0x82c3c8+0x10000
def tgt(a,w):
    li=w&0x3FFFFFC
    if li&0x2000000: li-=0x4000000
    return (a+li)&0xffffffff if not (w&2) else li
words=struct.unpack('>%dI'%((end-0x10000)//4),d[0:end-0x10000])
# stubs
stubs={}
for i,w in enumerate(words):
    a=0x10000+i*4
    if w==0xF8410028 and i+3<len(words) and (words[i+3]>>26)==18 and (words[i+1]>>26)==15:
        t=tgt(a+12,words[i+3])
        if t in tg: stubs[a]=t
print('stubs',{hex(k):hex(v) for k,v in stubs.items()})
allt=tg|set(stubs)
for i,w in enumerate(words):
    if (w>>26)==18:
        a=0x10000+i*4
        t=tgt(a,w)
        if t in allt and not (a in stubs or a-0 in stubs):
            f=fs[bisect.bisect_right(fs,a)-1]
            print(hex(a),'->',hex(t),'in',hex(f),'bl' if w&1 else 'b')
