"""Print the end address of the last executable section of a big-endian ELF64.

    python tools/elf_code_end.py elf/EBOOT.ELF      ->  0x0083A28C

This is the value for ppu_lifter.py --code-end: the highest executable address
(exclusive). A section is executable when its sh_flags has SHF_EXECINSTR (0x4).
For EBOOT.ELF (BCES00052 1.00) the executable sections are .text (0x10280..0x8393A8)
and the import stub sections (0x8393A8..0x83A28C, .sceStub.text); everything from
0x83A28C up to the end of the R-X segment (0x83C3C8) is read-only data.
"""
import struct
import sys

SHF_EXECINSTR = 0x4


def code_end(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 2:
        raise SystemExit("not a big-endian ELF64: " + path)
    e_shoff, = struct.unpack(">Q", d[0x28:0x30])
    e_shentsize, e_shnum = struct.unpack(">HH", d[0x3A:0x3E])
    if not e_shoff or not e_shnum:
        raise SystemExit("no section headers in " + path)
    end = 0
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        _name, _type, flags, addr, _off, size = struct.unpack(">IIQQQQ", d[o:o + 40])
        if flags & SHF_EXECINSTR and addr and size:
            end = max(end, addr + size)
    return end


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    print("0x%08X" % code_end(sys.argv[1]))
