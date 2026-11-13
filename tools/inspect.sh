#!/bin/bash
# Lists what to look at in a built WMVSlop.elf: symbols, the hook, and instructions a system thread shouldn't use.
cd "$(dirname "$0")/../build" || exit 1
T=/opt/xsdk-gcc/bin/powerpc-xsdk-elf-
"${T}nm" WMVSlop.elf | grep -E 'wmvslop_| U |resolve_all'
"${T}objdump" -d WMVSlop.elf > dis.txt
echo "--- vector instructions: $(grep -cE 'lvx|stvx|vrsave' dis.txt)"
echo "--- r13 / r2 uses:"
grep -E 'r13|[,(]r2[,)]' dis.txt | head -5
echo "--- hook"
grep -A 30 '<wmvslop_vdswap_hook>:' dis.txt | head -34
echo "--- first sled words"
grep -A 3 '<wmvslop_sled>:' dis.txt
echo "--- float conversions"
grep -E 'fcfid|frsp|lfs|lfd' dis.txt | head -12
echo "--- mnemonics"
awk -F'\t' 'NF>=3{split($3,a," "); print a[1]}' dis.txt | sort | uniq -c | sort -n | tr '\n' ' '
echo
