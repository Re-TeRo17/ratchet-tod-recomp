"""SELF -> ELF for the disc's EBOOT.BIN (an application SELF, key revision 0x0001).

The same steps as dod3-recomp's src/install/self.cpp: the metadata info is
opened with the application key set (AES-256-CBC), the metadata headers, the
section table and the section keys with the key it holds (AES-128-CTR), then
each program segment is decrypted and inflated into place behind the ELF's
own headers, which the SELF carries in the clear.

    python tools/self2elf.py <EBOOT.BIN> <out.ELF>
"""
import hashlib
import struct
import sys
import zlib

from Crypto.Cipher import AES
from Crypto.Util import Counter

# Application keys by key revision (rpcs3/Crypto/key_vault.cpp, KEY_APP).
APP_KEYS = {
    0x0001: (
        "79481839C406A632BDB4AC093D73D99AE1587F24CE7E69192C1CD0010274A8AB",
        "6F0F25E1C8C4B7AE70DF968B04521DDA",
    ),
}

SELF_MAGIC = b"SCE\0"


def be16(d, o): return struct.unpack_from(">H", d, o)[0]
def be32(d, o): return struct.unpack_from(">I", d, o)[0]
def be64(d, o): return struct.unpack_from(">Q", d, o)[0]


def ctr(key, iv, data):
    c = Counter.new(128, initial_value=int.from_bytes(iv, "big"))
    return AES.new(key, AES.MODE_CTR, counter=c).decrypt(data)


def self_to_elf(self):
    if self[:4] != SELF_MAGIC or be16(self, 0x0A) != 1:
        raise ValueError("not a SELF")
    key_rev = be16(self, 0x08)
    meta_off = be32(self, 0x0C)
    header_len = be64(self, 0x10)
    app_info = be64(self, 0x28)
    elf_off = be64(self, 0x30)
    phdr_off = be64(self, 0x38)
    shdr_off = be64(self, 0x40)
    self_type = be32(self, app_info + 0x0C)
    if self_type != 4:
        raise ValueError(f"not an application SELF (type {self_type})")
    if key_rev not in APP_KEYS:
        raise ValueError(f"no application keys for key revision 0x{key_rev:04X}")
    erk, riv = (bytes.fromhex(x) for x in APP_KEYS[key_rev])

    mi = 0x20 + meta_off
    meta = AES.new(erk, AES.MODE_CBC, riv).decrypt(self[mi:mi + 0x40])
    if any(meta[0x10:0x20]) or any(meta[0x30:0x40]):
        raise ValueError("the keys do not open this SELF")

    md = ctr(meta[0x00:0x10], meta[0x20:0x30], self[mi + 0x40:header_len])
    nsec, nkeys = be32(md, 0x0C), be32(md, 0x10)
    if 0x20 + nsec * 0x30 + nkeys * 16 > len(md):
        raise ValueError("the SELF's metadata is damaged")
    sec = 0x20
    dkeys = sec + nsec * 0x30

    eh = self[elf_off:elf_off + 0x40]
    if eh[:4] != b"\x7fELF" or eh[4] != 2 or eh[5] != 2:
        raise ValueError("the SELF does not hold a 64-bit big-endian ELF")
    e_phoff, e_shoff = be64(eh, 0x20), be64(eh, 0x28)
    e_phnum, e_shnum = be16(eh, 0x38), be16(eh, 0x3C)
    ph = self[phdr_off:phdr_off + e_phnum * 0x38]

    end = max(0x40, e_phoff + e_phnum * 0x38)
    if e_shnum:
        end = max(end, e_shoff + e_shnum * 0x40)
    for i in range(e_phnum):
        end = max(end, be64(ph, i * 0x38 + 0x08) + be64(ph, i * 0x38 + 0x20))
    elf = bytearray(end)
    elf[0:0x40] = eh
    elf[e_phoff:e_phoff + len(ph)] = ph
    if e_shnum:
        elf[e_shoff:e_shoff + e_shnum * 0x40] = self[shdr_off:shdr_off + e_shnum * 0x40]

    for i in range(nsec):
        s = sec + i * 0x30
        off, size = be64(md, s), be64(md, s + 8)
        typ, prog, encrypted = be32(md, s + 0x10), be32(md, s + 0x14), be32(md, s + 0x20)
        key_idx, iv_idx, compressed = be32(md, s + 0x24), be32(md, s + 0x28), be32(md, s + 0x2C)
        if typ != 2:  # 2: a program segment
            continue
        p_offset, p_filesz = be64(ph, prog * 0x38 + 0x08), be64(ph, prog * 0x38 + 0x20)
        data = self[off:off + size]
        if encrypted == 3:
            k = md[dkeys + key_idx * 16:dkeys + key_idx * 16 + 16]
            v = md[dkeys + iv_idx * 16:dkeys + iv_idx * 16 + 16]
            data = ctr(k, v, data)
        if compressed == 2:
            data = zlib.decompress(data)
            if len(data) != p_filesz:
                raise ValueError(f"segment {prog} inflates to {len(data)} bytes, not {p_filesz}")
        else:
            data = data[:p_filesz]
        elf[p_offset:p_offset + len(data)] = data
    return bytes(elf), key_rev


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        self = f.read()
    elf, key_rev = self_to_elf(self)
    with open(sys.argv[2], "wb") as f:
        f.write(elf)
    print(f"key revision 0x{key_rev:04X}, {len(elf):,} bytes")
    print(f"sha256 {hashlib.sha256(elf).hexdigest()}")


if __name__ == "__main__":
    main()
