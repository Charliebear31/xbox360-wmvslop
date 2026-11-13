/* Lays WMVSlop out for tools/mkxex.py (xsdk's elf2xex with a DLL's header): read-only data, code and writable
   data each start on their own 64 KB page. The base is passed as BASE (build.sh). */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(wmvslop_entry)

SECTIONS
{
	/* The packer puts the PE headers in the first 4 KB. */
	. = BASE + 0x1000;
	.rdata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		*(.sdata2 .sdata2.* .sbss2 .sbss2.*)
	}

	. = ALIGN(0x10000);
	.text : {
		/* The sled must start on a 64 KB boundary (src/hook.S). */
		*(.text.wmvslop_sled)
		*(.text.xsdk_thunk_*)
		*(.text .text.*)
	}

	. = ALIGN(0x10000);
	.data : {
		*(.data .data.* .gnu.linkonce.d.*)
		*(.sdata .sdata.*)
	}
	.bss : {
		*(.sbss .sbss.* .bss .bss.* COMMON)
	}

	.xsdk.imports 0 : {
		KEEP(*(.xsdk.imports))
	}

	/DISCARD/ : {
		*(.comment .note.* .gnu.attributes .eh_frame .eh_frame_hdr)
	}
}
