# Optional packed RomFS (development candidate)

This branch is unfinished: writer, reader, nn_fs file/directory paths, startup selection and native font reads have authored checks,
but broader image-backed compatibility validation is incomplete. Do not remove an extracted RomFS folder or
use this as a release feature.

The intended delivery is an optional read-only asset backend with the existing folder backend, mod ordering,
generated prompts, ultrawide redirects and native menu fonts preserved. Writable saves, cache and temporary mounts
stay separate. Loading-time benefits need comparable measurements; fewer host files alone does not prove faster
loading, lower memory use or less retained disk space.

## Writer contract

`python -I nx2/romfs_pack.py <folder> <output>` copies from an existing folder without changing it. The output must
be outside the source. Existing output is refused unless `--replace` is supplied. Temporary output is in the target
directory; errors/cancellation leave the prior output intact. Default publication refuses a competing output.

Only ordinary files/directories are accepted, including empty files/directories. Names are UTF-8 relative paths with
`/` separators; links/reparse points, case-ambiguous paths and Windows device/path aliases are refused. Files are
streamed, with identity/size/time checks before and after reads and a final directory inventory comparison. These
checks detect ordinary input changes, not an adversary modifying contents while restoring all observed metadata.
Copy from a stable verified/repaired folder. The build/update pipeline still requires its extracted directory.

## Candidate version 1 disk layout

All integers are unsigned little-endian. There are no native C structs on disk. The 64-byte header is followed by
file payloads in record order, then the index. The image has no trailing bytes or gaps between payloads.

| Offset | Bytes | Meaning |
|---:|---:|---|
|0|8|Magic: ASCII HROMFS1 followed by NUL|
|8|4|Entry count, at most1,000,000|
|12|4|Flags, zero in version1|
|16|8|Absolute index offset|
|24|8|Index byte length, at most64 MiB|
|32|8|Payload end, equal to index offset|
|40|8|Total file length|
|48|4|CRC32 of the index|
|52|4|CRC32 of the header, calculated with this field zero|
|56|8|Reserved, zero|

Each index record is24 bytes plus the UTF-8 name, without a NUL terminator:

| Offset | Bytes | Meaning |
|---:|---:|---|
|0|4|Name byte length,1..768|
|4|4|Type:0 file,1 directory|
|8|8|Absolute payload offset; directories use0|
|16|8|Payload byte length; directories use0|
|24|variable|Name bytes|

Records are strictly sorted by unsigned UTF-8 bytes. Parents are explicit directory records; the root is implicit.
File offsets begin at64 and advance by the preceding file size, including zero-length files. The complete image
must fit signed64-bit file offsets. Reader validation checks extents before payload reads. CRC32 detects accidental
metadata corruption; it is neither an authenticity guarantee nor a payload hash.

## Native reader contract

Desktop selection: pass `--romfs-pack <file>` to the native game executable. It consumes its own argument and opens
the image before guest memory is reserved. Missing/invalid/unreadable images stop startup with a diagnostic; remove
the option explicitly to return to the folder backend. `--romfs` retains its folder-root meaning for mounts/overrides.
Paths follow the normal startup working directory. Browser builds reject packed-image selection. Shader-only
preparation does not need the image. This branch still needs complete host/game and font-source validation before
these commands are suitable for a release.

`host/romfs_pack.h` owns a bounded immutable index and entry table. Its caller owns the backing file and supplies
positional reads with actual byte counts and a stable lifetime. Failed open leaves the caller's prior value unchanged; close frees
only the index/table. Reads clamp to a member boundary, accept EOF and reject directories/offsets past EOF.
The caller must validate destination buffers and keep the pack and backing file alive for all outstanding handles.
A short/error read reports bytes already written even when the operation fails, so the host can mark actual guest
memory writes correctly. Header/index reads must complete exactly before the image can be mounted.

`host/romfs_pack_file.h` supplies actual Win32 positional reads (the production POSIX layer on Linux). It owns one
read-only image handle and its validated index. Initialize it before readers start, keep it at a stable address and
close it only after every consumer stops. Windows sharing refuses writers; Linux replacement keeps an existing
reader on its open inode. In-place modification of an actively mounted image is unsupported.

Windows additionally builds a sorted UTF-16 name index once at open, using the OS ordinal case-insensitive comparator.
ASCII/Unicode case aliases then use binary search, and equivalent duplicate names reject the image. Directory
enumeration uses the canonical stored prefix. Linux retains exact byte lookup. Windows index allocations are bounded
by the entry/name limits above: UTF-16 capacity is at most the sum of UTF-8 name lengths plus one per entry, with
one name-record table and transient merge scratch. This adds metadata memory and startup sorting cost, not a memory
saving. Tests cover authored names and default Windows filesystem behavior, not every filesystem configuration.

Directory cursors are independent. Empty name opens root; other names must identify a directory. Enumeration returns
immediate children in index order, with basename lengths and file sizes. Mode1 selects directories,2 files,3 both.
Count reports the total for the mode without changing the open cursor, even after partial reads. Descendant ranges
are skipped by binary search while preserving siblings such as `dir.ext` and `dir0`.

Windows directory metadata may report an allocation size or zero between scans. The writer treats directory
payload size as zero while retaining identity, permissions, timestamps and complete membership checks. File size
and before/after read checks remain strict. A replaced directory still aborts publication and preserves prior output.

Still required before release: image-backed game and generated-prompt/mod integration validation, final exact-head
native build and full regressions. The host font callback uses renderer interface version19; explicit image selection
and authored Windows case/path parity are implemented. No image/index/game-derived fixture is shipped.
