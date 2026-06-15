#!/bin/sh
# Generate a Windows VERSIONINFO resource (.rc) for a bundled binary.
#
# Why this exists: unsigned mingw PE files with no publisher metadata are the
# textbook profile for Defender's `Wacatac.B!ml` false positive. Embedding real
# product metadata (CompanyName, ProductName, FileDescription, version) gives
# the binary an identity and measurably lowers the ML false-positive rate. It is
# NOT a full fix — Authenticode signing is — but it is free and helps.
#
# The version is read from the extension package.json so it stays single-sourced
# and never drifts from the VSIX version.
#
# Usage: gen-rc.sh <package.json> <out.rc> <internal-name> <file-description>
set -e

PKG_JSON="$1"
OUT="$2"
NAME="$3"
DESC="$4"

if [ -z "$PKG_JSON" ] || [ -z "$OUT" ] || [ -z "$NAME" ] || [ -z "$DESC" ]; then
    echo "usage: gen-rc.sh <package.json> <out.rc> <internal-name> <file-description>" >&2
    exit 1
fi

VER=$(grep -m1 '"version"' "$PKG_JSON" | sed -E 's/.*"version"[ ]*:[ ]*"([^"]+)".*/\1/')
[ -n "$VER" ] || { echo "gen-rc.sh: could not read version from $PKG_JSON" >&2; exit 1; }

MAJ=$(echo "$VER" | cut -d. -f1); MAJ=${MAJ:-0}
MIN=$(echo "$VER" | cut -d. -f2); MIN=${MIN:-0}
PAT=$(echo "$VER" | cut -d. -f3); PAT=${PAT:-0}

cat > "$OUT" <<RC
#include <winver.h>

VS_VERSION_INFO VERSIONINFO
 FILEVERSION ${MAJ},${MIN},${PAT},0
 PRODUCTVERSION ${MAJ},${MIN},${PAT},0
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS VOS_NT_WINDOWS32
 FILETYPE VFT_APP
 FILESUBTYPE VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName",      "Zerohexer"
            VALUE "FileDescription",  "${DESC}"
            VALUE "FileVersion",      "${VER}.0"
            VALUE "InternalName",     "${NAME}"
            VALUE "LegalCopyright",   "Copyright (C) Zerohexer. Licensed under MPL-2.0."
            VALUE "OriginalFilename", "${NAME}.exe"
            VALUE "ProductName",      "ZER Language"
            VALUE "ProductVersion",   "${VER}.0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
RC

echo "gen-rc.sh: wrote $OUT (version $VER) for $NAME.exe"
