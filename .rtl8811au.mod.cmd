savedcmd_rtl8811au.mod := printf '%s\n'   rtl8811au.o | awk '!x[$$0]++ { print("./"$$0) }' > rtl8811au.mod
