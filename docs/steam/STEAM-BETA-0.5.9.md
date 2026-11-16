# Steam Frame beta 0.5.9 compatibility

The signed beta payload `20261113.6134720` contains a new Android ARM64
`libsteamclient.so` (38,146,380 bytes), SHA-256
`807b50fe4bb34bd7de76180ed4a9ac305185ada8a51be354e93371aeff8a9786`.
It cannot use the 0.4.5/0.5.6–0.5.8 client profile.

## Native bridge audit

The `frame-beta-20261113` profile covers the interactive game-host session.
Each of its 13 mapped functions has the same unwind-defined function size and
complete instruction sequence as the recovery client after normalizing
PC-relative external branches, ADRP page references, and associated page
offsets. Internal branch distances and non-address instruction operands are
preserved in the comparison. The eight-byte slot-0 method has several identical
implementations; its identity was resolved using the full IClientUser vtable
sequence rather than a function signature alone.

Independent ELF `R_AARCH64_RELATIVE` relocation checks found the expected
IClientUser methods at vtable `0x227adb8`, slots 0, 1, 3, 4, 5, 6, 9, 54 and 56.
The engine getter is at slot 8 of vtable `0x228e630`. Dynamic symbols independently
confirm `Steam_LogOn=0xe73890`, `Steam_LogOff=0xe73924` and
`Steam_BConnected=0xe73a54`. The profile records the remaining method offsets.
The existing live image-owner and method/export-address checks remain active.
The credential-free WebUI transport diagnostic has no mapping for this client.

## Login UI

The receipt-verified UI uses loader `ze` and desktop entry module `43519`.
Packaging suppresses that desktop entry and exposes the existing module loader.
Authentication remains module `13459.kX`; the HTTPS service transport moved from
`79853.D` to `19099.x`. Valve's RSA helper is now private inside module `53366`,
whose exported `P8` function accepts a password and an object containing
`publickey_mod` and `publickey_exp`. A small adapter preserves the login wrapper's
existing helper interface and calls Valve's encryption function.

`steam/tests/t_steam_auth_ui_assets.cjs` checks the actual packaged modules
offline. It constructs QR, polling, RSA-key, credentials and Steam Guard
requests through Valve's generated service methods, instantiates the anonymous
HTTPS transport, and encrypts a fixture password with a generated RSA key.
Node decrypts that fixture to verify the helper's argument and padding contract.
No account sign-in or network request is performed.

```sh
node steam/tests/t_steam_auth_ui_assets.cjs /path/to/packaged/SteamAuthAssets
python3 -m unittest discover -s steam/tests -p 'test_steam_*.py'
```

## Existing downloads

`resolve()` rechecks a previously incompatible receipt against the current
bridge after validating every payload file. `status()` uses that result, so
**Refresh versions** recognizes the installed beta without another download.
The immutable download receipt and channel pointer are preserved. Unknown
client hashes, changed payload files and unrecognized UI bootstraps still fail
validation.

These static and offline checks do not establish native authentication,
headset execution, or game-server ticket acceptance for this beta. Those require
an installed-app run.
