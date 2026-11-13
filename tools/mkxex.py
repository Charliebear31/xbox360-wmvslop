"""Packs WMVSlop's ELF as a system DLL .xex: xsdk's elf2xex with a different image base and module type.

python mkxex.py <xsdk tools dir> <base hex> <in.elf> <out.xex>

xsdk's elf2xex writes a title (module flag 1) at 0x82000000. A DashLaunch plugin is a DLL (module flag 8,
XEX_MODULE_FLAG_DLL_MODULE) in the system's address range, so both are overridden before its main() runs."""
import importlib.util
import struct
import sys
from pathlib import Path

MODULE_DLL = 0x00000008

tools, base, elf, xex = sys.argv[1:5]
spec = importlib.util.spec_from_file_location("elf2xex", Path(tools) / "elf2xex.py")
elf2xex = importlib.util.module_from_spec(spec)
spec.loader.exec_module(elf2xex)
elf2xex.IMAGE_BASE = int(base, 16)
elf2xex.MODULE_TITLE = MODULE_DLL
# Addresses from 0x90000000 up are mapped in 4 KB pages, and the kernel refuses an image whose page size
# doesn't fit its address (0xC000007B, the user's console, 2026-11-12, with 64 KB pages at 0x91E40000). Every
# system module in update 16197 has 4 KB page descriptors and image flags 0x30000000 (tools/xexinfo.py).
elf2xex.PAGE_BYTES = 0x1000
# Those modules' PE headers also carry IMAGE_FILE_DLL (0x2000).
elf2xex.PE_EXECUTABLE_IMAGE = 0x2002
sys.argv = ["elf2xex.py", elf, xex, "--title-id", "0", "--stack-size", "0x10000"]
elf2xex.main()

# elf2xex writes image flags 0. They sit in the part of the security info that only the signature covers
# (the header digest starts after it), so setting them afterwards changes no digest.
IMAGE_FLAGS_SMALL_PAGES_REGION_FREE = 0x30000000
data = bytearray(Path(xex).read_bytes())
security = struct.unpack_from(">I", data, 0x10)[0]
assert struct.unpack_from(">I", data, security + 0x110)[0] == elf2xex.IMAGE_BASE
struct.pack_into(">I", data, security + 0x10C, IMAGE_FLAGS_SMALL_PAGES_REGION_FREE)
Path(xex).write_bytes(data)
