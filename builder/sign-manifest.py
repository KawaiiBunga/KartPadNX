#!/usr/bin/env python3
"""
Sign manifest.json with an Ed25519 private key.

Writes the detached signature file manifest.json.sig: base64 of the Ed25519
signature over the exact manifest bytes. Every signature is checked against
the public key the launcher pins in launcher/source/net/release_key.h, unless
--public-key names a test key instead.

  sign-manifest.py manifest.json [--key KEY] [--output SIG]
  sign-manifest.py manifest.json --verify-only [--output SIG]
  sign-manifest.py --gen-key KEY      # test key pair: KEY (seed) + KEY.pub
"""

import argparse
import base64
import os
import re
import sys
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519


def load_public_key_from_header(header_path: str) -> bytes:
    if not os.path.isfile(header_path):
        raise FileNotFoundError(f"Header file not found: {header_path}")
    with open(header_path, "r", encoding="utf-8") as f:
        content = f.read()
    # Find bytes like 0x12, 0xab
    m = re.search(r'kReleasePublicKey\[32\]\s*=\s*\{([^}]+)\}', content)
    if not m:
        raise ValueError(f"Could not find kReleasePublicKey array in {header_path}")
    byte_strs = re.findall(r'0x([0-9a-fA-F]{2})', m.group(1))
    if len(byte_strs) != 32:
        raise ValueError(f"Expected 32 hex bytes in {header_path}, found {len(byte_strs)}")
    return bytes(int(b, 16) for b in byte_strs)


def load_public_key(public_key_path, header_path):
    """The raw 32-byte test key when one is given, otherwise the pinned release key."""
    if public_key_path:
        with open(public_key_path, "rb") as f:
            raw = f.read()
        if len(raw) != 32:
            raise ValueError(f"Expected a 32-byte raw Ed25519 public key in {public_key_path}, got {len(raw)}")
        return ed25519.Ed25519PublicKey.from_public_bytes(raw), public_key_path
    return ed25519.Ed25519PublicKey.from_public_bytes(load_public_key_from_header(header_path)), header_path


def generate_key(key_path):
    """Writes a new 32-byte private seed to key_path and its raw public key to key_path.pub."""
    pub_path = key_path + ".pub"
    for path in (key_path, pub_path):
        if os.path.exists(path):
            sys.stderr.write(f"ERROR: Refusing to overwrite existing key file: {path}\n")
            sys.exit(1)
    priv_key = ed25519.Ed25519PrivateKey.generate()
    seed = priv_key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                  serialization.NoEncryption())
    pub = priv_key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    key_dir = os.path.dirname(key_path)
    if key_dir:
        os.makedirs(key_dir, exist_ok=True)
    with open(key_path, "wb") as f:
        f.write(seed)
    with open(pub_path, "wb") as f:
        f.write(pub)
    print(f"==> Wrote private seed {key_path} and public key {pub_path}")
    print(f"    Public key: {pub.hex()}")


def main():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    default_key = os.path.join(repo_root, "private", "release", "mkw-release-ed25519.key")
    default_header = os.path.join(repo_root, "launcher", "source", "net", "release_key.h")

    parser = argparse.ArgumentParser(description="Sign manifest.json with Ed25519 private key")
    parser.add_argument("manifest", nargs="?", help="Path to manifest.json file to sign")
    parser.add_argument("--key", "-k", default=default_key,
                        help=f"Path to Ed25519 private key (default: {default_key})")
    parser.add_argument("--output", "-o", default=None,
                        help="Output path for signature file (default: <manifest>.sig)")
    parser.add_argument("--verify-only", action="store_true",
                        help="Only verify existing signature without signing")
    parser.add_argument("--public-key", default=None,
                        help="Raw 32-byte public key to verify against instead of release_key.h (test keys only)")
    parser.add_argument("--gen-key", metavar="KEY", default=None,
                        help="Generate a test key pair (KEY and KEY.pub) and exit")

    args = parser.parse_args()

    if args.gen_key:
        generate_key(args.gen_key)
        return
    if not args.manifest:
        parser.error("manifest is required unless --gen-key is given")

    if not os.path.isfile(args.manifest):
        sys.stderr.write(f"ERROR: Manifest file not found: {args.manifest}\n")
        sys.exit(1)

    sig_path = args.output or f"{args.manifest}.sig"

    with open(args.manifest, "rb") as f:
        raw_manifest_bytes = f.read()

    if args.verify_only:
        if not os.path.isfile(sig_path):
            sys.stderr.write(f"ERROR: Signature file not found: {sig_path}\n")
            sys.exit(1)
        with open(sig_path, "r", encoding="utf-8") as f:
            sig_b64 = f.read().strip()
        sig_bytes = base64.b64decode(sig_b64)
        pub_key, pub_source = load_public_key(args.public_key, default_header)
        try:
            pub_key.verify(sig_bytes, raw_manifest_bytes)
            print(f"VERIFIED: {sig_path} is valid for {args.manifest} ({pub_source})")
            sys.exit(0)
        except Exception as e:
            sys.stderr.write(f"ERROR: Verification failed: {e}\n")
            sys.exit(1)

    # Sign mode
    if not os.path.isfile(args.key):
        sys.stderr.write(f"ERROR: Ed25519 private key file not found: {args.key}\n")
        sys.exit(1)

    with open(args.key, "rb") as f:
        priv_bytes = f.read()

    if len(priv_bytes) != 32:
        sys.stderr.write(f"ERROR: Expected 32-byte Ed25519 private seed, got {len(priv_bytes)} bytes\n")
        sys.exit(1)

    priv_key = ed25519.Ed25519PrivateKey.from_private_bytes(priv_bytes)
    signature = priv_key.sign(raw_manifest_bytes)
    sig_b64 = base64.b64encode(signature).decode('ascii') + "\n"

    with open(sig_path, "w", encoding="utf-8") as f:
        f.write(sig_b64)

    print(f"==> Signed {args.manifest} -> {sig_path}")
    print(f"    Signature: {sig_b64.strip()}")

    # A signature the launcher would reject is an error, not a warning.
    pub_key, pub_source = load_public_key(args.public_key, default_header)
    try:
        pub_key.verify(signature, raw_manifest_bytes)
        print(f"==> Self-verification against {pub_source}: PASSED (key matches)")
    except Exception as e:
        sys.stderr.write(f"ERROR: Self-verification against {pub_source} failed: {e}\n")
        sys.exit(1)


if __name__ == "__main__":
    main()
