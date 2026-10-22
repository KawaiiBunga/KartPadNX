# Writes a translation unit holding this link's UTC build time. Run on every
# build (see FullApp.cmake) so the boot log names the exact NRO that ran.
string(TIMESTAMP _stamp "%Y-%m-%dT%H:%M:%SZ" UTC)
file(WRITE "${OUT}.tmp" "const char kartpad_build_stamp[] = \"KARTPAD-BUILD ${_stamp}\";\n")
file(RENAME "${OUT}.tmp" "${OUT}")
