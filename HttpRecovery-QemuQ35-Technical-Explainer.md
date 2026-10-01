# Testing DFCI HTTP Recovery with QemuQ35

Date: 2026-10-01

## Purpose

This document explains how we exercised the real DFCI Refresh From Network
implementation in QemuQ35, including:

- Creating a valid enrolled DFCI state.
- Running the same recovery implementation used by the firmware menu.
- Hosting the DFCI HTTP and HTTPS endpoints directly in WSL.
- Persisting state across multiple QEMU cold resets.
- Supplying a test TLS certificate and signed recovery packets.
- Switching between OneCrypto binaries without changing the test.
- Proving success from captured requests and final firmware state.

The implementation is split across two repositories:

- [`mu_tiano_platforms`](./) supplies the QemuQ35 firmware
  build profile.
- [`Features/DFCI`](./Features/DFCI) supplies the
  DFCI applications, certificate and packet generators, Refresh Server, shell
  state machine, and host orchestrator.

For the OneCrypto investigation and A/B results, see
[`DFCI-OneCrypto-Investigation.md`](./DFCI-OneCrypto-Investigation.md).

## What is being tested

The test does not call a mock TLS function or reproduce the recovery protocol
in a test application. It runs `DfciSARecovery.efi`, which enters the same
DFCI network-recovery implementation used by the firmware menu:

```c
Status = GetDfciParameters ();

if (!CheckIfDfciEnrolled ()) {
  return EFI_SUCCESS;
}

if (mDfciMenuConfiguration.DfciHttpRecoveryEnabled == MENU_TRUE) {
  IssueDfciNetworkRequest ();
}
```

Source:
[`DfciSARecovery.c`](./Features/DFCI/DfciPkg/Application/DfciMenu/DfciSARecovery.c)

The resulting firmware path is:

```text
DfciSARecovery.efi
  -> DFCI recovery settings and identity protocols
  -> DfciRecoveryLib
  -> EFI_HTTP_PROTOCOL / HttpDxe
  -> EFI_TLS_PROTOCOL / TlsDxe for HTTPS
  -> BaseCryptLibOnOneCrypto
  -> OneCrypto / OpenSSL
  -> WSL Refresh Server
```

The test therefore covers much more than a TLS handshake:

- DFCI identity and permission state.
- Recovery URL and certificate settings.
- JSON request generation.
- HTTP and HTTPS transport.
- Asynchronous `202 Accepted` plus `Location` polling.
- Signed response validation and application.
- Persistent variable changes across reboot.
- Final owner and user unenrollment.

## Why a pristine QEMU boot is insufficient

DFCI recovery is stateful. `DfciSARecovery.efi` will not issue a network
request simply because the application exists in the virtual drive.

The system must first be enrolled, and HTTP recovery requires all of these
settings:

- Recovery permission.
- HTTPS recovery URL.
- HTTPS certificate.
- Tenant ID.
- Registration ID.

The firmware explicitly gates recovery on those values:

```c
Status = DfciGetASetting (
           DFCI_PRIVATE_SETTING_ID__DFCI_RECOVERY_URL,
           DFCI_SETTING_TYPE_STRING,
           (VOID **)&mDfciUrl,
           &mDfciUrlSize
           );
if (EFI_ERROR (Status) || (mDfciUrlSize <= sizeof (CHAR8))) {
  goto NO_HTTP_RECOVERY;
}

Status = DfciGetASetting (
           DFCI_PRIVATE_SETTING_ID__DFCI_HTTPS_CERT,
           DFCI_SETTING_TYPE_CERT,
           (VOID **)&mDfciNetworkRequest.HttpsCert,
           &mDfciNetworkRequest.HttpsCertSize
           );
if (EFI_ERROR (Status) ||
    (mDfciNetworkRequest.HttpsCertSize <= sizeof (CHAR8))) {
  goto NO_HTTP_RECOVERY;
}
```

The same function checks the tenant ID and registration ID, validates that
the certificate has a thumbprint, parses the recovery URL, and only then sets:

```c
mDfciMenuConfiguration.DfciHttpRecoveryEnabled = MENU_TRUE;
```

This is why the test first applies signed owner and user packets instead of
trying to invoke recovery on a pristine variable store.

## End-to-end architecture

```text
Windows tools                         WSL
-------------                         ---
Python packet generators  <-------->  RunQemuRecoveryTest.py
Windows SDK signtool.exe               |
Git openssl.exe                        +--> Generate test fixtures
                                       +--> Start Flask/CherryPy server
                                       +--> Create OneCrypto ext_dep override
                                       +--> Build QemuQ35
                                       +--> Run QEMU
                                                 |
                                                 v
                                      QemuQ35 UEFI shell
                                                 |
                                         startup.nsh
                                                 |
                    +----------------------------+-------------------------+
                    |                            |                         |
              DfciApply.efi               DfciSARecovery.efi       cold reset
                    |                            |
            signed enrollment             HTTP / HTTPS stack
                    |                            |
            persistent variables                 +--> 10.0.2.2:8080
                                                 +--> 10.0.2.2:8443
```

QEMU uses user-mode networking. From the guest, `10.0.2.2` addresses the host
of the QEMU process. Because QEMU runs in WSL, the Refresh Server must be
reachable in WSL; launching it only on Windows does not make it the guest's
slirp host.

## 1. Add a default-off QemuQ35 test profile

The platform changes are gated behind `DFCI_NETWORK_RECOVERY_TEST`, which
defaults to `FALSE`:

```ini
!ifndef DFCI_NETWORK_RECOVERY_TEST
  DEFINE DFCI_NETWORK_RECOVERY_TEST = FALSE
!endif
```

Source:
[`QemuQ35PkgCommon.dsc.inc`](./Platforms/QemuQ35Pkg/QemuQ35PkgCommon.dsc.inc)

This ensures a normal QemuQ35 build does not acquire test certificates,
unattended enrollment behavior, or HTTP bootstrap policy.

When enabled, the profile adds the shell applications:

```ini
!if $(DFCI_NETWORK_RECOVERY_TEST) == TRUE
  DfciPkg/Application/DfciApply/DfciApply.inf
  DfciPkg/Application/DfciMenu/DfciSARecovery.inf {
    <LibraryClasses>
      DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  }
!endif
```

The console `DebugLib` binding makes DFCI diagnostics visible in the captured
QEMU run log.

## 2. Enable the HTTP bootstrap path

DFCI uses an initial HTTP bootstrap before the HTTPS recovery request. QemuQ35
did define a network-related build setting, but the generated build report
still showed `PcdAllowHttpConnections = 0`.

The test profile sets the fixed PCD explicitly:

```ini
!include NetworkPkg/NetworkFixedPcds.dsc.inc
!if $(DFCI_NETWORK_RECOVERY_TEST) == TRUE
  gEfiNetworkPkgTokenSpaceGuid.PcdAllowHttpConnections|TRUE
!endif
```

Without this setting, the test cannot complete the first HTTP phase.

## 3. Package the matching test ZTD certificate

Owner enrollment is signed through the test ZTD chain. The firmware must
contain the matching ZTD certificate:

```ini
FILE FREEFORM = PCD(gZeroTouchPkgTokenSpaceGuid.PcdZeroTouchCertificateFile) {
!if $(DFCI_NETWORK_RECOVERY_TEST) == TRUE
    SECTION RAW = DfciPkg/UnitTests/DfciTests/Certs/ZTD_Leaf.cer
!else
    SECTION RAW = ZeroTouchPkg/Certs/ZeroTouch/ZtdRecovery.cer
!endif
}
```

Source:
[`QemuQ35Pkg.fdf`](./Platforms/QemuQ35Pkg/QemuQ35Pkg.fdf)

The production/default certificate remains unchanged when the test profile is
disabled.

## 4. Make enrollment unattended without weakening production builds

The normal QEMU DFCI UI asks the operator to confirm the final two
certificate-thumbprint digits. That is appropriate interactively but blocks
an unattended multi-reboot test.

The test profile selects a dedicated library:

```ini
!if $(DFCI_NETWORK_RECOVERY_TEST) == TRUE
  DfciUiSupportLib |
    QemuPkg/Library/DfciUiSupportLibTest/DfciUiSupportLibTest.inf
!else
  DfciUiSupportLib |
    QemuPkg/Library/DfciUiSupportLib/DfciUiSupportLib.inf
!endif
```

The test implementation auto-confirms certificate enrollment but refuses
password authentication:

```c
EFI_STATUS
EFIAPI
DfciUiDisplayAuthDialog (
  // Parameters omitted
  )
{
  if (PasswordType) {
    return EFI_UNSUPPORTED;
  }

  *Result = DFCI_MB_IDOK;
  if (Password != NULL) {
    *Password = NULL;
  }

  return EFI_SUCCESS;
}
```

Source:
[`DfciUiSupportLibTest.c`](./QemuPkg/Library/DfciUiSupportLibTest/DfciUiSupportLibTest.c)

This is test-only behavior. It must remain inaccessible to production builds.

## 5. Generate a certificate QEMU can validate

The HTTPS server is addressed by IP as `10.0.2.2`. A certificate that places
that value in a DNS SAN is not equivalent to an IP SAN.

The certificate generator detects whether the configured host is an IP
address:

```python
hostname = config['DfciTest']['server_host_name']
config_file.write('DNS.1 = localhost' + os.linesep)
try:
    ipaddress.ip_address(hostname)
except ValueError:
    config_file.write(f'DNS.2 = {hostname}' + os.linesep)
else:
    config_file.write(f'IP.1 = {hostname}' + os.linesep)
```

Source:
[`MakeHTTPSCert.py`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/Certs/MakeHTTPSCert.py)

The generator creates an RSA-2048 server certificate:

```python
command1 = [
    'openssl.exe',
    'req',
    '-x509',
    '-nodes',
    '-sha256',
    '-days', '3652',
    '-newkey', 'rsa:2048',
    '-keyout', f'{name}.key',
    '-out', f'{name}.pem',
    '-config', 'httpreq.cnf',
]
```

That RSA-2048 certificate is intentional for the OneCrypto compatibility
test. It passes OpenSSL security level 2 and fails security level 3.

## 6. Generate signed DFCI enrollment and response packets

The host orchestrator creates:

- Owner identity provisioning packet.
- Owner permission packet.
- Owner settings packet.
- User identity provisioning packet.
- User permission packet.
- User settings packet.
- Bootstrap server response.
- Recovery server response.

The generated settings replace the Docker-oriented URLs with QEMU slirp
addresses:

```python
pattern = pattern.replace(
    "http://host.docker.internal.com",
    f"http://{host_name}:{http_port}",
)
pattern = pattern.replace(
    "https://host.docker.internal.com",
    f"https://{host_name}:{https_port}",
)
```

The HTTPS certificate is embedded into the settings XML:

```python
run_windows(
    "python.exe ..\\..\\Support\\Python\\InsertCertIntoXML.py "
    "--BinFilePath ..\\..\\Certs\\DFCI_HTTPS.cer "
    "--OutputFilePath DfciSettingsQemu.xml "
    "--PatternFilePath DfciSettingsQemuPattern.xml",
    ENROLL_DIR,
)
```

Packet generation uses the existing DFCI Windows Python tools and Windows SDK
signing flow. For example, owner enrollment is signed by the expected ZTD and
DDS test identities:

```python
"python.exe ..\\..\\Support\\Python\\GenerateCertProvisionData.py "
"--CertFilePath ..\\..\\Certs\\DDS_CA.cer "
"--Step2AEnable --Signing2APfxFile ..\\..\\Certs\\DDS_Leaf.pfx "
"--Step2BEnable --Step2Enable "
"--SigningPfxFile ..\\..\\Certs\\ZTD_Leaf.pfx "
"--Step3Enable "
"--FinalizeResultFile OwnerEnroll_Provision_apply.bin "
"--Step1Enable --Identity 1 --HdrVersion 2 ..."
```

Source:
[`RunQemuRecoveryTest.py`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/RunQemuRecoveryTest.py)

These are real signed DFCI packets, not direct variable edits.

## 7. Run the Refresh Server directly in WSL

The existing Refresh Server was designed around Docker paths and ports 80/443.
The harness makes its paths and ports configurable so it can run directly in
WSL without root:

```python
server_data_dir = os.environ.get(
    'DFCI_REFRESH_DATA_DIR',
    '/srv/dfci_refresh_server/src',
)

server_http.socket_port = int(os.environ.get('DFCI_HTTP_PORT', '80'))
server_https.socket_port = int(os.environ.get('DFCI_HTTPS_PORT', '443'))
```

The runner supplies:

```python
server_env.update(
    {
        "DFCI_REFRESH_DATA_DIR": str(SERVER_SRC_DIR),
        "DFCI_TEST_CONFIG": str(config_path),
        "DFCI_SSL_DIR": str(CERTS_DIR),
        "DFCI_HTTP_PORT": str(http_port),
        "DFCI_HTTPS_PORT": str(https_port),
    }
)
```

The defaults used by the harness are:

```text
HTTP:  10.0.2.2:8080
HTTPS: 10.0.2.2:8443
```

### TLS policy of the test server

The HTTPS endpoint is constrained to TLS 1.2 and the ECDHE-RSA cipher suites
used by the DFCI service:

```python
ctx.set_max_proto_version(SSL.TLS1_2_VERSION)
ctx.set_min_proto_version(SSL.TLS1_2_VERSION)

cipher_list = [
    b'TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384',
    b'TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256',
    b'TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384',
    b'TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256',
]
ctx.set_cipher_list(b':'.join(cipher_list))
```

Source:
[`server.py`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/RefreshServer/Src/server.py)

## 8. Implement both asynchronous recovery exchanges

DFCI recovery uses two request/poll exchanges.

### HTTP bootstrap

The firmware first sends a JSON POST over HTTP:

```text
POST http://10.0.2.2:8080/
  ztd/unauthenticated/dfci/recovery-bootstrap
```

The server saves the body as `Bootstrap_Request.json`, returns
`202 Accepted`, and supplies a polling URL:

```python
filename = 'Bootstrap_Request.json'
pathname = os.path.join(
    dfci_refresh_server.config['REQUEST_FOLDER'],
    filename,
)
with open(pathname, 'wb') as file:
    file.write(request.data)

response = jsonify()
response.status_code = 202
response.headers['Location'] = (
    get_server_origin('http')
    + '/ztd/unauthenticated/dfci/'
      'recovery-bootstrap-status/request-id'
)
return response
```

DFCI follows the `Location` with an HTTP GET. The server returns the signed
bootstrap response, which establishes the transition to the recovery phase.

### HTTPS recovery

After the bootstrap response has been applied and the system reboots, DFCI
sends the recovery JSON over HTTPS:

```text
POST https://10.0.2.2:8443/
  ztd/unauthenticated/dfci/recovery-packets
```

The server captures `Recovery_Request.json`, returns another
`202 Accepted`, and directs the firmware to the signed recovery response:

```python
filename = 'Recovery_Request.json'
pathname = os.path.join(
    dfci_refresh_server.config['REQUEST_FOLDER'],
    filename,
)
with open(pathname, 'wb') as file:
    file.write(request.data)

response = jsonify()
response.status_code = 202
response.autocorrect_location_header = False
response.headers['Location'] = (
    get_server_origin('https')
    + '/ztd/unauthenticated/dfci/'
      'recovery-packets-status/request-id'
)
return response
```

The polling endpoint returns `Recovery_Response.json`, whose signed packets
remove the owner and user identities.

Source:
[`main.py`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/RefreshServer/Src/main.py)

### Why redirect URLs use configured ports

The UEFI HTTP client's `Host` header did not preserve the nonstandard port in
a form Flask could use to reconstruct the origin. Building `Location` from
`request.host_url` sent the polling request to port 80 or 443, causing a
connection failure after a successful POST.

The fix was to construct the origin explicitly:

```python
def get_server_origin(scheme):
    if scheme == 'http':
        port = int(os.environ.get('DFCI_HTTP_PORT', '80'))
        default_port = 80
    elif scheme == 'https':
        port = int(os.environ.get('DFCI_HTTPS_PORT', '443'))
        default_port = 443
    else:
        raise ValueError(f'Unsupported URL scheme {scheme}')

    port_suffix = '' if port == default_port else f':{port}'
    return f'{scheme}://{get_host_name()}{port_suffix}'
```

This preserves normal 80/443 behavior while supporting 8080/8443.

## 9. Use a multi-reboot UEFI shell state machine

DFCI packet application and recovery alter persistent firmware state and
require reboots. One QEMU process is run with a persistent variable store and
a writable virtual drive. Marker files on that drive select the next stage:

```nsh
if not exist owner-applied.txt then
  DfciApply.efi -v \
    -i OwnerEnroll_Provision_apply.bin \
    -p OwnerPermissions_Permission_apply.bin \
    -s OwnerSettings_Settings_apply.bin
  echo done > owner-applied.txt
  reset -c
endif

if not exist user-applied.txt then
  DfciApply.efi -v \
    -i UserEnroll_Provision_apply.bin \
    -p UserPermissions_Permission_apply.bin \
    -s UserSettings_Settings_apply.bin
  echo done > user-applied.txt
  reset -c
endif

if not exist bootstrap-complete.txt then
  DfciSARecovery.efi
  echo done > bootstrap-complete.txt
  reset -c
endif

if not exist recovery-complete.txt then
  DfciSARecovery.efi
  echo done > recovery-complete.txt
  reset -c
endif
```

Source:
[`QemuRecoveryStartup.nsh`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/RefreshServer/QemuRecoveryStartup.nsh)

The stages are:

| Boot | Action | Persistent result |
|---:|---|---|
| 1 | Apply owner packets | Owner identity, permissions, and network settings |
| 2 | Apply user packets | User identity, permissions, and settings |
| 3 | Run recovery | HTTP bootstrap request and response application |
| 4 | Run recovery again | HTTPS recovery request and signed response application |
| 5 | Final boot | Verify owner/user removal, then shut down |

### Marker files are sequencing, not proof

The shell writes a marker after invoking each application; it does not parse
the application's status. This keeps the UEFI script simple, but a marker
alone does not prove network success.

The host-side orchestrator therefore requires independent evidence:

- `Bootstrap_Request.json` must exist.
- `Recovery_Request.json` must exist.
- The firmware log must show owner and user enrolled before recovery.
- The firmware log must show owner and user absent after recovery.
- The QEMU run must end with `PROGRESS - Success`.

## 10. Boot the shell without losing networking

The build/run command uses:

```python
[
    stuart_build,
    "-c", "Platforms/QemuQ35Pkg/PlatformBuild.py",
    "TARGET=DEBUG",
    "TOOL_CHAIN_TAG=GCC",
    "BLD_*_DFCI_NETWORK_RECOVERY_TEST=TRUE",
    "BLD_*_GUI_FRONT_PAGE=TRUE",
    "BLD_*_QEMU_CORE_NUM=2",
    "ENABLE_NETWORK=TRUE",
    "BOOT_TO_FRONT_PAGE=FALSE",
    "EMPTY_DRIVE=TRUE",
    "SHUTDOWN_AFTER_RUN=TRUE",
    f"STARTUP_NSH={STARTUP_NSH}",
    "FILE_REGEX=DfciApply.efi,DfciSARecovery.efi,*_apply.bin",
    "--FlashRom",
]
```

Two details are important:

- `ENABLE_NETWORK=TRUE` includes and starts the QEMU network stack.
- `BOOT_TO_FRONT_PAGE=FALSE` allows the internal shell to execute
  `startup.nsh`. Setting it to `TRUE` bypassed the shell even though the
  network device was present.

`TOOL_CHAIN_TAG=GCC` is required by this checkout; `GCC5` is not configured.

## 11. Swap OneCrypto through a verified ext_dep override

The test accepts a OneCrypto ZIP as an input. It hashes the ZIP and creates a
temporary ext-dependency override:

```python
sha256 = hashlib.sha256(onecrypto_zip.read_bytes()).hexdigest()
manifest = {
    "scope": "global",
    "type": "web",
    "id": "onecrypto-bin-local-override",
    "name": "onecrypto-bin-local-override",
    "override_id": "onecrypto-bin",
    "source": onecrypto_zip.resolve().as_uri(),
    "version": f"1.0.0-dfci-recovery-{sha256[:12]}",
    "sha256": sha256,
    "compression_type": "zip",
    "internal_path": "/",
    "flags": ["set_build_var"],
    "var_name": "BLD_*_ONE_CRYPTO_PATH",
}
```

`stuart_update` consumes the override and verifies the hash. The runner saves
and restores any pre-existing override, then runs `stuart_update` again during
cleanup.

This made the OneCrypto A/B comparison controlled: the platform, server,
certificate, packets, and recovery flow stayed constant while only the
OneCrypto ZIP changed.

## 12. Validate behavior, not just process exit

The orchestrator preserves the firmware log and both network requests:

```python
evidence = {
    platform_root / "Build" / "BUILDLOG_QemuQ35Pkg_Run.txt":
        "QemuRecovery.log",
    SERVER_SRC_DIR / "Requests" / "Bootstrap_Request.json":
        "Bootstrap_Request.json",
    SERVER_SRC_DIR / "Requests" / "Recovery_Request.json":
        "Recovery_Request.json",
}

for source, name in evidence.items():
    if not source.exists():
        raise RuntimeError(
            f"Expected evidence was not produced: {source}"
        )
    shutil.copy2(source, output_dir / name)
```

It then checks state transitions:

```python
required_markers = (
    "OwnerEnabled=1, UserEnabled=1",
    "OwnerEnabled=0, UserEnabled=0",
    "PROGRESS - Success",
)

missing = [
    marker for marker in required_markers
    if marker not in log_text
]
if missing:
    raise RuntimeError(
        f"Firmware log is missing success markers: {missing}"
    )
```

This is why the official OneCrypto v1.1.0 run correctly fails the harness even
though QEMU itself reaches a normal shutdown:

- The HTTP bootstrap request exists.
- The TLS handshake aborts during server-certificate processing.
- `Recovery_Request.json` does not exist.
- Owner and user remain enrolled.

## One-command execution

Run the script from WSL using a Python environment that has Flask, CherryPy,
and PyOpenSSL:

```bash
python DfciPkg/UnitTests/DfciTests/RunQemuRecoveryTest.py \
  --platform-root /home/doug/git/maintence/mu_tiano_platforms \
  --output-dir \
    DfciPkg/UnitTests/DfciTests/Artifacts/QemuRecovery
```

The checked-in ext-dependency supplies the diagnostic OneCrypto binary. Add
`--onecrypto-zip /path/to/OneCrypto-X64.zip` to test a different local drop.

The default network settings are:

```text
QEMU host address: 10.0.2.2
HTTP port:         8080
HTTPS port:        8443
```

The script must be run from WSL, but it invokes Windows tools for certificate
and signed-packet generation. Required tools are:

- WSL Python with Flask, CherryPy, and PyOpenSSL.
- Windows Python with the DFCI test requirements.
- Windows SDK `signtool.exe`.
- Git for Windows `openssl.exe`.
- A configured Stuart environment, default `/opt/venv`.

## Safety and cleanup

Before generating anything, the runner refuses to overwrite:

- `DfciTests.ini`.
- Generated HTTPS certificate files.
- Generated enrollment XML.
- Generated packet binaries.
- Existing server `Requests` or `Responses` directories.

The guard is explicit:

```python
conflicts = [
    path for path in generated_paths
    if path.exists()
]
if conflicts:
    raise RuntimeError(
        "Generated test paths already exist. "
        "Move or remove them before running:\n"
        + "\n".join(f"  {path}" for path in conflicts)
    )
```

On exit, including failures, the `finally` block:

1. Terminates the Refresh Server.
2. Restores or removes the OneCrypto override.
3. Refreshes external dependencies.
4. Removes generated certificates, packets, XML, requests, and responses
   unless `--keep-generated` was specified.

Evidence copied to the selected artifact directory remains available.

## Expected success evidence

A passing run contains:

```text
CheckIfDfciEnrolled ... OwnerEnabled=1, UserEnabled=1
...
CheckIfDfciEnrolled ... OwnerEnabled=0, UserEnabled=0
...
PROGRESS - Success
```

The artifact directory contains:

```text
QemuRecovery.log
Bootstrap_Request.json
Recovery_Request.json
```

For the validated diagnostic OneCrypto run, these are under:

[`Artifacts/QemuRecovery`](./Features/DFCI/DfciPkg/UnitTests/DfciTests/Artifacts/QemuRecovery)

## Expected TLS-failure evidence

With the platform-pinned official OneCrypto v1.1.0 and the RSA-2048 server
certificate, the flow reaches the HTTPS phase and reports:

```text
TlsDoHandshake SSL_HANDSHAKE_ERROR State=0x4 SSL_ERROR_SSL
TlsDoHandshake ERROR 0xA000086=L14:R86
tls_post_process_server_certificate()
HttpNotify: Event - 2, EventStatus - Aborted
HttpNotify: Event - 3, EventStatus - Aborted
```

The artifact directory contains `Bootstrap_Request.json` but not
`Recovery_Request.json`, and the final state remains:

```text
OwnerEnabled=1, UserEnabled=1
```

That evidence proves:

1. Signed enrollment worked.
2. HTTP bootstrap worked.
3. The server and QEMU network path were reachable.
4. The failure occurred while validating the HTTPS server certificate.
5. The signed recovery response was never requested or applied.

## What this test proves

A passing run proves that the tested firmware and OneCrypto binary can:

- Establish the required DFCI enrollment state.
- Persist owner and user identities across reboot.
- Read DFCI recovery settings.
- Generate the bootstrap and recovery JSON payloads.
- Complete the HTTP asynchronous bootstrap flow.
- Complete the HTTPS TLS and asynchronous recovery flow.
- Validate and apply signed recovery packets.
- Persist the recovered state.
- Remove owner and user identities.

It is therefore a full firmware integration test of DFCI network recovery,
not merely an HTTP reachability or TLS handshake test.

## Test-only boundaries

The following must not be interpreted as production behavior:

- `DFCI_NETWORK_RECOVERY_TEST` is a default-off QEMU profile.
- `DfciUiSupportLibTest` auto-confirms certificate enrollment.
- The test ZTD certificate is packaged only in the gated profile.
- The diagnostic OneCrypto binary disables certificate-time validation.
- Generated test identities, certificates, and signed packets are fixtures.

The QEMU harness is suitable for repeatable compatibility and regression
testing. Production firmware must use its normal UI, trust anchors,
certificate-time validation, and platform security policy.
