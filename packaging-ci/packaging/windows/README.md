# Windows cross-package builder

This directory defines an **unsigned Linux cross-build**, not a native Windows
build. The content-addressed Fedora 44 image pins MinGW GCC 16.1.1, Qt 6.11.1,
Rust 1.95.0, NSIS 3.11, msitools/wixl 0.106.58, and Wine 11. Fedora's MinGW Qt
multimedia package omits the `QtMultimedia` QML plugin, so the image builds only
that missing module from the official Qt 6.11.1 source tarball after verifying
its SHA-256 checksum. The target remains `x86_64-pc-windows-gnu` throughout.

`installer.nsi` creates a setup executable that installs per-user by default
and per-machine on request (install-mode page, or `/ALLUSERS`; see
`docs/windows-packaging.md`, "Install scope"). It never asks for elevation up
front (`RequestExecutionLevel user`) and relaunches itself through UAC only for
an all-users install. The MSI generator uses a stable UpgradeCode, a ProductCode
derived from application version plus source commit, and component GUIDs derived
from normalized staged paths; it is per-user by default and per-machine with
`ALLUSERS=1`. Both installers write `.lightning-install-scope` for the updater
and preserve application user data during uninstall.

The runtime stager copies the target Qt QML tree, selected runtime plugins,
and then closes the DLL import graph recursively. It does not copy the Qt SDK.
`validate-windows-artifacts.sh` checks PE architecture/import closure, required
QML/plugins, MSI tables, forbidden paths/secrets, and hashes. Wine tests are
supplemental only; native Windows behavior remains untested.

`native-windows-acceptance.ps1` is the native check, run by hand on a real
Windows host against the installed artifact. Each check ends PASS, FAIL or
NOT TESTED; the verdict is PASS (exit 0) only when every required check ran and
passed, FAIL (exit 1) on any failure, and INCONCLUSIVE (exit 2) when a required
check could not run, such as an unreadable PE header. Runtime checks need
positive log lines (the `windows` platform, the Rust backend, the Windows
credential store), not just the absence of errors, and a normal close must end
the process without a kill. `acceptance-results.txt` records the counts, the
SHA-256 of the `Lightning.exe` it ran and the build's source commit. Run it in
a disposable Windows user account: the account cache opens only after a
sign-in, so that check stays NOT TESTED on a fresh profile and is optional.

Component inventory: `write-build-info.sh` records one for each `.deb` only
(every ELF file in the payload mapped to a component or `unknown`, the declared
system dependencies, and every `Cargo.lock` package). The Windows bundle,
whose Qt, GStreamer and MinGW DLLs are bundled rather than system packages, has
no inventory yet, and there is no advisory triage step; neither may be assumed.
