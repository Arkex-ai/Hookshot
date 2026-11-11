# AppImage package bootstrap recovery

The Ubuntu22.04 CI container first refreshes its base indexes and installs the tools needed to enroll the
toolchain PPA. This phase retries independently, at most three times.

The PPA phase then enrolls or repairs the repository before explicitly refreshing its signed indexes and installing
the toolchain/dependencies. Enrollment uses `--no-update` to separate these operations. It does not bypass signature
validation: dependent packages are installed only after the signed refresh succeeds.

This ordering matters after a failed signing-key request (#2266). Enrollment may already have written the source;
an initial `apt-get update` then rejects its missing key. Repeating that refresh before the enrollment repair cannot
recover, even if Launchpad is available again. Each PPA-phase retry starts with the operation that can repair it.

Both phases retain bounded retries,20second delays and the final failing status. No repository/key deletion or apt
authentication override is used. The target remains the same ephemeral CI container and the same package set.

`python -I -B tests/test_appimage_package_retry.py` executes the workflow's real shell under `sh -e` with authored
command stand-ins. They model a source left behind without its key, require valid signed indexes before installation,
and exercise transient/persistent failures at every phase boundary. No real network or package manager is run.
