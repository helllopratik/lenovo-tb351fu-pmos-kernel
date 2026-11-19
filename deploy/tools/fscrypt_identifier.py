#!/usr/bin/env python3
"""Derive the fscrypt v2 master-key identifier from a raw key file.

This mirrors fs/crypto/keyring.c and fs/crypto/hkdf.c:

  PRK   = HMAC-SHA512(key=64 zero bytes, IKM=master_key)
  info  = "fscrypt\0" || HKDF_CONTEXT_KEY_IDENTIFIER (1)
  ident = HMAC-SHA512(PRK, info || 0x01)[:16]

The key is read from a 0600 file so it never appears in a process command
line.  Usage: fscrypt_identifier.py KEYFILE
"""

import hashlib
import hmac
import sys


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: fscrypt_identifier.py KEYFILE")
    with open(sys.argv[1], "r", encoding="ascii") as fh:
        raw = fh.read().strip()
    try:
        key = bytes.fromhex(raw)
    except ValueError:
        sys.exit("invalid hexadecimal key")
    if not 16 <= len(key) <= 64:
        sys.exit("fscrypt v2 master key must be 16-64 bytes")

    prk = hmac.new(b"\x00" * 64, key, hashlib.sha512).digest()
    info = b"fscrypt\x00" + bytes([1])
    ident = hmac.new(prk, info + b"\x01", hashlib.sha512).digest()[:16]
    print(ident.hex())


if __name__ == "__main__":
    main()
