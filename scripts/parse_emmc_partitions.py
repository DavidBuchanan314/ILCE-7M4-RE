import os
import sys
from enum import Enum
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

# input is a raw encrypted emmc dump, either with physical emmc reader or from /dev/nflasha (which symlinks to /dev/pnflasha)
src, dst_dir = sys.argv[1], sys.argv[2]
emmc = open(src, "rb")

def u32(buf: bytes, off: int):
    return int.from_bytes(buf[off:off+4], "little")

SECTOR = 512

class PartType(Enum):
    EXT2 = 4
    FAT  = 5
    WBI2 = 6

here = os.path.dirname(__file__)
brom = open(os.path.join(here, "../bootrom/CXD90057_Mira_0xFFFF0000.bin"), "rb").read()
loader = open(os.path.join(here, "../emmc_boot/esram_0xfe040000.bin"), "rb").read()


# key derivation (0x239b0 is hardcoded, could vary by emmc bootloader version? across models?)
offset_table = [u32(loader, 0x239b0 + i) >> 16 for i in range(0, 0x100, 4)]
scramble_pad = brom[0xb800:0xbc00]
key = bytes(scramble_pad[i] for i in offset_table)

print("AES-XTS key:")
print(key[:32].hex())
print(key[32:].hex())

# parse SDM2 partition table (see linux-kernel/block/partitions/sdm2_partition_table.h )
magic = emmc.read(4)
assert magic == b"8246"
version = emmc.read(4)
assert version == b"2.00"
n_partition = int.from_bytes(emmc.read(4), "little")

for part_i in range(n_partition):
    emmc.seek(32 + part_i * 16)
    start_sector = int.from_bytes(emmc.read(4), "little")
    sector_count = int.from_bytes(emmc.read(4), "little")
    type = PartType(int.from_bytes(emmc.read(4), "little"))
    flag = int.from_bytes(emmc.read(4), "little")

    if not flag & 1: # SDM_LABEL_VALID
        continue

    devname = f"nflasha{part_i + 1}"  # ew, 1-indexing

    print()
    print(f"=== partition {part_i} ===")
    print(f"{devname       = }")
    print(f"{start_sector  = :#x}")
    print(f"{sector_count  = :#x}")
    print(f"{type          = }")
    print(f"{flag          = :#x}")

    emmc.seek(SECTOR * start_sector)
    with open(os.path.join(dst_dir, devname), "wb") as outfile:
        for i in range(sector_count):
            tweak = (start_sector + i // SECTOR).to_bytes(16, "little")
            dec = Cipher(algorithms.AES(key), modes.XTS(tweak)).decryptor()
            sector = dec.update(emmc.read(SECTOR)) + dec.finalize()
            outfile.write(sector)
