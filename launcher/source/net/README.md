# Launcher updater: release signing

The updater (`update_service.cpp`) downloads two files from the latest GitHub release:

| File | Content |
|---|---|
| `manifest.json` | release metadata: `schema` (1), `version`, `channel`, `released`, `changelog`, and `components` with the launcher's `file`, `size`, `sha256` and `url` |
| `manifest.json.sig` | base64 (standard alphabet) of the 64-byte Ed25519 signature of the **exact bytes** of `manifest.json`; trailing whitespace is ignored |

It verifies the signature against the public key pinned in `release_key.h` before parsing the
manifest, then checks the downloaded NRO's size and SHA-256 against the manifest. A release
build refuses to compile with `MKW_UPDATER_ALLOW_UNSIGNED`.

Never edit or reformat `manifest.json` after signing: the signature covers its raw bytes.

## Making a release

`builder/build-release.sh` builds the game-free launcher, then:

```bash
python3 builder/make-manifest.py build/release/<tag>/mkw-launcher.nro <version> --output manifest.json
python3 builder/sign-manifest.py manifest.json --key <private key> --output manifest.json.sig
python3 builder/sign-manifest.py manifest.json --output manifest.json.sig --verify-only
```

The private key stays outside the repository. `sign-manifest.py --gen-key KEY` makes a throwaway
test key pair (CI uses one with `--public-key`); a test-key signature does not verify against the
pinned release key.
