savedcmd_rtl8811au.ko := ld -r -m elf_x86_64 -z noexecstack --no-warn-rwx-segments --build-id=sha1  -T /usr/lib/modules/6.13.7-hardened1-1-hardened/build/scripts/module.lds -o rtl8811au.ko rtl8811au.o rtl8811au.mod.o .module-common.o
