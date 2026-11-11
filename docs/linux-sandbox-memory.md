# Guest-memory selection with hidden module parameters

Some Linux sandboxes expose `/dev/udmabuf` but hide `/sys/module`. Previously, startup treated an unavailable
`size_limit_mb` file as a disabled device and chose anonymous guest memory. On AMD, that can select the slower
host-pointer import path even when dma-bufs work.

When the device opens but its size-limit file cannot be opened, startup now permits the memfd path using the same
conservative 64 MB fallback already used by the renderer. This is a compatibility bound, not a claim about the
machine's actual module setting or every kernel's default. A readable positive value is still used; readable zero
or contents that cannot be parsed still disables automatic memfd selection. Device permission failures retain their existing
warning and fallback. `HOS_GUEST_MEMFD=0` and the existing explicit force behavior are unchanged.

The renderer still checks imports and uses its existing sparse-piece/failure paths. This change does not configure
permissions, alter pool policy or change page discard. #2411 reports a separate memfd-discard crash, including a
rendererless arm; this startup change does not fix it. Hardware validation must check actual imports and area
transitions as well as frame rate. Controlled source tests do not establish affected Flatpak/AMD stability.

`tests/test_udmabuf_selection.py` runs the production selection block with authored device/sysfs boundaries,
including explicit overrides, zero/invalid limits and device failure. `--mutations` checks five independently
removed policy guards. The permission warning retains its own regression test. Neither test opens a real device.
