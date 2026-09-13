"""
Decrypt the next boot stage out of emmc_boot/emmc_boot0.bin, like the bootrom does.

Produces emmc_boot/esram_0xfe040000.bin, which can be loaded into ghidra etc. at addr 0xfe040000
"""

import os
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

def u32(buf: bytes, off: int):
    return int.from_bytes(buf[off:off+4], "little")

SECTOR = 512

here = os.path.dirname(__file__)
brom = open(os.path.join(here, "../bootrom/CXD90057_Mira_0xFFFF0000.bin"), "rb").read()
boot0 = open(os.path.join(here, "../emmc_boot/emmc_boot0.bin"), "rb").read()

# parse "information sect{ion,or}" (guessed name)
assert boot0.startswith(b"EXBL")

# as read by the bootrom during "normal" boot
start_sector = u32(boot0, 0x68)
load_addr = u32(boot0, 0x6c)
load_len = u32(boot0, 0x74)
entrypoint = u32(boot0, 0x78)

print(f"{start_sector  = :#x}")
print(f"{load_addr     = :#x}")
print(f"{load_len      = :#x}")
print(f"{entrypoint    = :#x}")
print()

# loaded into eSRAM unconditionally at offset 0xfe04_0000
esram = boot0[:0x800]

# means we can trivially concat the decrypted payload to the esram buf
assert load_addr == 0xfe04_0800

emmc_loader_enc = boot0[start_sector*SECTOR:start_sector*SECTOR + load_len]

# key derivation
offset_table = [u32(brom, 0x6984 + i) for i in range(0, 0x100, 4)]
scramble_pad = brom[0xb800:0xbc00]
key = bytes(scramble_pad[i] for i in offset_table)

print("AES-XTS key:")
print(key[:32].hex())
print(key[32:].hex())

for i in range(0, len(emmc_loader_enc), SECTOR):
    tweak = (start_sector + i // SECTOR).to_bytes(16, "little")
    dec = Cipher(algorithms.AES(key), modes.XTS(tweak)).decryptor()
    esram += dec.update(emmc_loader_enc[i:i + SECTOR]) + dec.finalize()

with open(os.path.join(here, "../emmc_boot/esram_0xfe040000.bin"), "wb") as outfile:
    outfile.write(esram)