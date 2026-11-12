# Pipeline cache file storage

Vulkan pipeline cache persistence uses file-backed contiguous storage rather than a
temporary byte vector the size of the cache. This applies to the initial base
cache, play-generation files, startup compaction and subsequent base-cache saves.
The existing Vulkan cache format, generation retention, growth threshold and
`RX_VK_PCACHE_LOAD` / `RX_VK_PLAY_CACHE` switches are unchanged.

The small device/driver header is checked before acquiring the complete payload.
Windows holds a read-only mapping with write sharing denied. Linux first copies
the accepted file into an owned, unlinked sibling temporary file with a bounded
buffer, then maps that private inode. Later replacement or truncation of the
original pathname cannot invalidate that snapshot. Incomplete acquisition is
discarded. Linux therefore needs temporary disk space and permission to create a
file beside the cache; refusal leaves the cache empty for that read.

Saving creates a unique owned sibling temporary, reserves the queried capacity
and lets Vulkan write directly into its mapped bytes. Only `VK_SUCCESS` with a
valid actual length permits flush, unmap, truncation, close and atomic replacement.
`VK_INCOMPLETE`, failed I/O or denied replacement leaves the old published cache
and saved-size accounting intact, so a later save can retry. There is no in-place
overwrite fallback. On Windows a mapped destination can prevent replacement;
the old cache is retained until the reader releases it and a later save succeeds.

This removes the temporary bulk private/anonymous allocation, not the Vulkan
driver's retained cache. File-backed pages still consume physical memory and I/O.
The Linux snapshot adds disk traffic; filesystem-dependent acquisition timings
are not a game startup or frame-rate promise. Published files must be replaced
atomically, and source bytes should remain stable during snapshot acquisition.
Private snapshots do not protect against media failures: mapped I/O can raise
native in-page faults, which are not ordinary recoverable C++ I/O exceptions.

Game-free checks:

```sh
python -I -B tests/run_all.py pipeline_cache pipeline_play_cache pcache_header
```

These compile actual production functions/helpers, exercise native file ownership
and injected syscall refusals, and reject semantic mutations for incorrect saved
length, unchecked headers and leaked abandoned temporaries. Actual driver and
game runs remain separate local validation; no generated cache files belong in
the repository.
