# OneCrypto TLS Policy and DFCI Network Recovery Investigation

Date: 2026-09-30

## Executive summary

OneCrypto's explicit OpenSSL security level 3 policy prevents DFCI Refresh From
Network from connecting to an RSA-2048 server certificate. Controlled QemuQ35
tests established the following:

- Security level 3 rejects RSA-2048 with X.509 error 66,
  `EE certificate key too weak`.
- Security level 2 accepts RSA-2048.
- Security level 3 accepts RSA-3072.
- Security level 2 continues to reject RSA-1024.
- The platform-pinned official OneCrypto v1.1.0 binary completes DFCI
  enrollment and HTTP bootstrap, but aborts while processing the HTTPS server
  certificate. It never sends the HTTPS recovery request and leaves owner and
  user identities enrolled.
- An ABI-compatible diagnostic binary using security level 2 and
  `X509_V_FLAG_NO_CHECK_TIME` completes the full signed DFCI network recovery
  flow and removes both identities.

The diagnostic binary is not a production solution because it disables
certificate-time validation. The preferred production direction is to use
OpenSSL's compiled default security level 2, matching current EDK2 behavior,
while retaining normal certificate-time validation.

## Scope

This work covered:

1. Identifying the TLS policy regression.
2. Building matched OneCrypto policy variants.
3. Testing RSA-1024, RSA-2048, and RSA-3072 controls.
4. Adding OneCrypto ABI v1.1 forwarding for `TlsSetSecurityLevel`.
5. Producing and publishing an ABI-compatible diagnostic binary.
6. Testing the focused HTTPS/HTTP-429 path.
7. Testing the complete signed DFCI Refresh From Network workflow.
8. Building a repeatable, default-off QemuQ35 test harness.
9. Comparing the diagnostic binary with the platform-pinned official binary.

## Root cause

The relevant OneCrypto change is `mu_crypto_release` commit
`0f497b329e39e6317210d7f1bbad6bbd82e73e75`, which changed each new TLS
connection from OpenSSL security level 0 to level 3:

```diff
- SSL_set_security_level (TlsConn->Ssl, 0);
+ SSL_set_security_level (TlsConn->Ssl, 3);
```

At OpenSSL security level 3, RSA keys must provide approximately 128 bits of
security, which generally requires RSA-3072. Existing DFCI infrastructure may
use RSA-2048 certificates, which meet security level 2 but not level 3.

The failure occurs on this ownership path:

```text
DFCI
  -> EFI_HTTP_PROTOCOL / HttpDxe
  -> EFI_TLS_PROTOCOL / TlsDxe
  -> internal TLS connection
  -> OneCrypto / OpenSSL
```

DFCI cannot directly call `TlsSetSecurityLevel()` because it does not receive
the internal OpenSSL-backed TLS connection pointer.

## EDK2 precedent

Two upstream EDK2 changes are relevant:

- `fb43f0c085045771bc2dee2f867d87298de2facb` added
  `TlsSetSecurityLevel()`.
- `b3f19fb5ce9e9214a74c71c1993023977b26c919` removed the explicit level-3
  default and allowed OpenSSL's compiled default, security level 2, to apply.

This supports using level 2 as the default compatibility policy rather than
hard-coding level 3 for every firmware TLS client.

## Confirmed policy matrix

Certificate-time validation was disabled in both sides of the matched policy
comparison so key-strength behavior could be isolated.

| OneCrypto policy | Certificate | Result |
|---|---|---|
| Security level 3 | RSA-1024 | Fail |
| Security level 3 | RSA-2048 | Fail: X.509 error 66, weak key |
| Security level 3 | RSA-3072 | Pass: HTTPS completes, HTTP 429 received |
| Security level 2 | RSA-1024 | Fail: X.509 error 66, weak key |
| Security level 2 | RSA-2048 | Pass: HTTPS completes, HTTP 429 received |
| Security level 2 | RSA-3072 | Pass |

This demonstrates that level 2 restores RSA-2048 compatibility without
accepting RSA-1024.

## OneCrypto changes

### Diagnostic branch

Repository:
[`mu_crypto_release`](../mu_crypto_release)

Branch:
`test/dfci-level2-no-time-check`

Commit:
`4f22447fa41c370737f5401328e0196eb90ab2b1`
(`OpensslPkg: Add DFCI diagnostic TLS policy`)

The diagnostic policy is visible in
[`TlsInit.c`](../mu_crypto_release/OpensslPkg/Library/TlsLib/TlsInit.c):

```c
SSL_set_security_level (TlsConn->Ssl, 2);
```

The same file adds `X509_V_FLAG_NO_CHECK_TIME` to the TLS verification
parameters. Other X.509 and PKCS7 verification paths were also relaxed for the
diagnostic build.

These time-check changes were made only to isolate the key-strength policy.
They must not be used in production.

### Published diagnostic binary

Release:
[`v1.1.1-dfci-level2-notime.0`](https://github.com/Flickdm/mu_crypto_release/releases/tag/v1.1.1-dfci-level2-notime.0)

Asset:
`OneCrypto-X64-DEBUG-level2-no-time-check.zip`

SHA-256:

```text
e375712b58d5419503260ab87a983620d9df820f1a01a7c2e79542f251dd51b9
```

The binary preserves the OneCrypto 1.0 ABI so it can be substituted into the
existing platform without requiring a protocol update.

### `TlsSetSecurityLevel` ABI work

The consumer-side OneCrypto protocol v1.1 forwarding was implemented in
`MU_BASECORE`:

Branch:
`fix/tls-security-level`

Commit:
`81c85f9d063a7063a600cf9807093c768416f28e`
(`CryptoPkg: Expose TLS security level through OneCrypto`)

This work exposes `TlsSetSecurityLevel` through the OneCrypto protocol and
`BaseCryptLibOnOneCrypto`. It is useful when an explicit platform policy is
required, but DFCI itself still cannot set the value per request.

## Focused HTTPS test

`DfciCheck429.efi` was used to exercise the firmware HTTP/TLS path without
requiring DFCI enrollment.

The probe:

1. Loads the test CA certificate.
2. Connects to the Refresh Server over HTTPS.
3. Requests the `/return_429` endpoint.
4. Passes only when the firmware receives HTTP status 429.

The diagnostic level-2 binary passed with RSA-2048. The level-3 build failed
certificate verification with error 66.

The focused probe changes remain in the detached
[`Features/DFCI`](./Features/DFCI) workspace:

- `DfciPkg/Application/DfciMenu/DfciCheck429.c`
- `DfciPkg/Application/DfciMenu/DfciCheck429.inf`
- `DfciPkg/Application/DfciMenu/DfciRequest.c`
- `DfciPkg/Application/DfciMenu/DfciRequest.h`

## Complete DFCI Refresh From Network test

The full test exercises the same recovery implementation as the firmware
menu, using `DfciSARecovery.efi`.

The automated sequence is:

1. Opt in the test ZTD certificate.
2. Apply signed owner enrollment, permissions, and settings.
3. Apply signed user enrollment, permissions, and settings.
4. Send the HTTP bootstrap request.
5. Apply the returned transition/settings packets.
6. Reboot.
7. Send the HTTPS recovery request.
8. Apply the signed recovery response.
9. Reboot and verify that owner and user identities were removed.

### Full-flow A/B result

The same RSA-2048 IP-SAN certificate, server, packets, QEMU platform changes,
and test sequence were used for both runs. Only the OneCrypto ZIP changed.

| Evidence | Official OneCrypto v1.1.0 | Diagnostic level-2/no-time-check |
|---|---|---|
| Signed owner enrollment | Pass | Pass |
| Signed user enrollment | Pass | Pass |
| HTTP bootstrap request | Captured | Captured |
| HTTPS recovery request | Not produced | Captured |
| TLS result | Certificate verification abort | Handshake succeeds |
| Final owner state | Still enrolled | Removed |
| Final user state | Still enrolled | Removed |
| Harness result | Fail | Pass |

The official binary reported:

```text
TlsDoHandshake SSL_HANDSHAKE_ERROR State=0x4 SSL_ERROR_SSL
TlsDoHandshake ERROR 0xA000086=L14:R86
tls_post_process_server_certificate()
HttpNotify: Event - 2, EventStatus - Aborted
HttpNotify: Event - 3, EventStatus - Aborted
```

The official binary does not print `SSL_get_verify_result()`, so the full-flow
log alone reports generic certificate verification failure. The instrumented
focused policy matrix provides the specific weak-key attribution.

### Official binary tested

The tested QemuQ35 checkout declares the official binary in
[`OneCrypto_ext_dep.json`](./MU_BASECORE/CryptoPkg/Binaries/OneCrypto_ext_dep.json):

```text
Release: v1.1.0-OneCrypto
Asset:   OneCrypto-Accelerated.zip
SHA-256: e18de6ed1019bdb9dd156d1a218af1534592291e76b376377a7b4c8fa5a95470
```

The downloaded asset hash was verified before the test.

## Repeatable test harness changes

The repeatable harness is split between `mu_tiano_platforms` and
`Features/DFCI`.

### `mu_tiano_platforms`

#### QemuQ35 build profile

[`QemuQ35PkgCommon.dsc.inc`](./Platforms/QemuQ35Pkg/QemuQ35PkgCommon.dsc.inc)
adds a default-off `DFCI_NETWORK_RECOVERY_TEST` build setting.

When enabled, it:

- Selects the test-only DFCI UI support library.
- Sets `gEfiNetworkPkgTokenSpaceGuid.PcdAllowHttpConnections` to `TRUE`.
- Builds `DfciApply.efi`.
- Builds `DfciSARecovery.efi` with console debug output.

[`QemuQ35Pkg.fdf`](./Platforms/QemuQ35Pkg/QemuQ35Pkg.fdf)
packages the test `ZTD_Leaf.cer` only when the profile is enabled. Normal
builds continue to package the production recovery certificate.

#### Test-only UI

[`DfciUiSupportLibTest`](./QemuPkg/Library/DfciUiSupportLibTest)
implements unattended certificate enrollment for the QEMU test:

- Reports manufacturing mode.
- Auto-confirms certificate-thumbprint prompts.
- Rejects password prompts.

This library is gated by `DFCI_NETWORK_RECOVERY_TEST` and must never be
selected by a production build.

### `Features/DFCI`

The primary entry point is
[`RunQemuRecoveryTest.py`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/RunQemuRecoveryTest.py).

It:

1. Refuses to overwrite existing generated fixtures or request data.
2. Generates an IP-SAN HTTPS certificate.
3. Generates signed server responses.
4. Generates signed owner and user enrollment packets.
5. Creates a local OneCrypto ext-dependency override with a verified SHA-256.
6. Starts the Refresh Server on WSL ports 8080 and 8443.
7. Runs `stuart_update`.
8. Builds the gated QemuQ35 profile.
9. Copies the shell applications and packets to the virtual drive.
10. Runs the multi-reboot firmware flow.
11. Verifies request evidence and enrollment-state markers.
12. Restores the prior OneCrypto override and removes generated inputs.

The UEFI shell state machine is
[`QemuRecoveryStartup.nsh`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/RefreshServer/QemuRecoveryStartup.nsh).

Additional server/test changes:

- `MakeHTTPSCert.py` emits an IP SAN when the configured host is an IP
  address.
- `GenResponses.bat` uses the tracked `Certs` and `Src` casing required by
  WSL.
- `main.py` supports configurable data/config paths and generates async
  redirect URLs with explicit configured ports.
- `server.py` supports configurable SSL paths and HTTP/HTTPS ports.
- `.gitignore` excludes generated recovery evidence.
- `readme.md` documents prerequisites, execution, evidence, and cleanup.

### Important redirect finding

The UEFI HTTP request's `Host` header omits a nonstandard port. Deriving an
async `Location` header from Flask's `request.host_url` therefore redirects
the status poll to port 80 or 443 rather than 8080 or 8443.

The server must construct redirect origins from the configured host and
explicit HTTP/HTTPS ports.

## Running the repeatable test

Run from WSL with a Python environment containing Flask, CherryPy, and
PyOpenSSL:

```bash
python DfciPkg/UnitTests/DfciTests/RunQemuRecoveryTest.py \
  --platform-root /home/doug/git/maintence/mu_tiano_platforms \
  --output-dir DfciPkg/UnitTests/DfciTests/Artifacts/QemuRecovery
```

The published diagnostic OneCrypto binary is selected by the branch's checked-in
ext-dependency. Use `--onecrypto-zip /path/to/OneCrypto-X64.zip` to temporarily test
another local drop.

The Windows side must provide:

- Python with the DFCI test requirements.
- A Windows SDK containing `signtool.exe`.
- Git for Windows. The runner locates Git's `openssl.exe` automatically.

The Stuart environment defaults to `/opt/venv`. Use `--stuart-venv` to
select another environment.

## Test evidence

### Passing diagnostic run

Directory:
[`QemuRecovery`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/Artifacts/QemuRecovery)

Files:

- `QemuRecovery.log`
- `Bootstrap_Request.json`
- `Recovery_Request.json`

Verified markers:

```text
OwnerEnabled=1, UserEnabled=1
OwnerEnabled=0, UserEnabled=0
PROGRESS - Success
```

### Failing official v1.1.0 run

Directory:
[`QemuRecovery-OneCrypto-v1.1.0`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/Artifacts/QemuRecovery-OneCrypto-v1.1.0)

Files:

- `QemuRecovery.log`
- `Bootstrap_Request.json`

`Recovery_Request.json` is intentionally absent because certificate
verification failed before the HTTPS POST.

Final state:

```text
OwnerEnabled=1, UserEnabled=1
```

## Validation performed

| Validation | Result |
|---|---|
| Python syntax compilation | Pass |
| Windows OpenSSL discovery through WSL | Pass |
| Certificate and signed-packet generation | Pass |
| Local file URI OneCrypto ext-dependency override | Pass |
| QemuQ35 build with `DFCI_NETWORK_RECOVERY_TEST=TRUE` | Pass |
| Complete multi-reboot QEMU run with diagnostic binary | Pass |
| Normal QemuQ35 DEBUG build with profile disabled | Pass |
| HTTP bootstrap on port 8080 | Pass |
| HTTPS recovery on port 8443 with diagnostic binary | Pass |
| Official v1.1.0 negative control | Pass: expected TLS failure observed |
| Evidence preservation | Pass |
| Generated-state cleanup | Pass |
| Previous OneCrypto override restoration | Pass |
| Diff whitespace checks | Pass |

## QEMU and build findings

- Use `TOOL_CHAIN_TAG=GCC`; `GCC5` is not configured in this checkout.
- `BOOT_TO_FRONT_PAGE=FALSE` preserves the virtio NIC while allowing
  `startup.nsh` to execute. `TRUE` bypasses the shell.
- Merely defining `NETWORK_ALLOW_HTTP_CONNECTIONS=TRUE` did not set
  `PcdAllowHttpConnections`; the test profile must set the PCD explicitly.
- QEMU slirp host `10.0.2.2` refers to the host of the QEMU process. Because
  QEMU runs in WSL, the Refresh Server must also be reachable from WSL.
- Ports 8080 and 8443 avoid requiring WSL root.
- QemuQ35 uses `QemuPkg/Library/DfciUiSupportLib`, not
  `DfciUiSupportLibNull`, so the test must override the active library
  instance.
- `BOOT_TO_FRONT_PAGE=TRUE` and a pristine variable store are insufficient
  for this test. Signed owner/user enrollment must be staged before recovery.

## Production recommendation

1. Do not ship the diagnostic binary.
2. Do not disable certificate-time validation.
3. Prefer the EDK2 behavior: remove the explicit level-3 override and use the
   compiled OpenSSL default, security level 2.
4. If an explicit policy is required, expose `TlsSetSecurityLevel` through
   OneCrypto ABI v1.1 and apply a platform-wide policy in `TlsDxe` before the
   handshake.
5. Inventory every certificate in the production DFCI chain. A 3072-bit leaf
   is insufficient if an intermediate or CA remains RSA-2048.
6. Retain negative controls:
   - RSA-1024 must fail.
   - Invalid time must fail.
   - Incorrect SAN/hostname must fail.
   - Incorrect EKU or trust chain must fail.

## Current repository state

### `mu_crypto_release`

```text
Branch: test/dfci-level2-no-time-check
HEAD:   4f22447fa41c370737f5401328e0196eb90ab2b1
State:  clean
```

### `mu_tiano_platforms`

```text
Branch: main
HEAD:   82a93e09bbb46b9623bcbc4ab35a6838cc58ff21
```

The QemuQ35 recovery-profile changes are uncommitted. `MU_BASECORE` currently
points at the local `fix/tls-security-level` commit
`81c85f9d063a7063a600cf9807093c768416f28e`.

### `Features/DFCI`

```text
State: detached HEAD
HEAD:  cd33bec609f4d55dc753eb7b485ea251a5fb6be6
```

The focused HTTPS probe and repeatable recovery-harness changes are
uncommitted.

## Conclusion

The existing official OneCrypto binary fails the complete DFCI recovery flow
at server-certificate verification when the endpoint uses RSA-2048. The
matched policy tests identify security level 3 as the compatibility
regression. Security level 2 restores RSA-2048 compatibility while preserving
rejection of RSA-1024.

The complete recovery harness now provides a repeatable A/B test that proves
behavior beyond TLS: request generation, async redirects, signed packet
processing, persistent enrollment state, and final recovery application.
