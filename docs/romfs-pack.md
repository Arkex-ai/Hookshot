# Optional packed RomFS

The native desktop game can read assets from an optional indexed image. Keep the extracted RomFS folder:
building and updating still require it. Image creation copies the assets and does not remove the source.

This is an optional read-only asset backend with the existing folder backend, mod ordering,
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
preparation does not need the image.

For example, from a built game folder on Windows:

```powershell
bin/hookshot.exe image.bin --romfs-pack game.hromfs --user user
```

On Linux use `bin/hookshot` with the same arguments. The required `image.bin` contains the native game's
program data; `game.hromfs` is the separate asset container. Generated prompt assets remain beside the game.
The outer launcher does not automatically select this backend: pass the flag to the native executable.
Close the game before replacing its packed image.

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

## Validation and limits

On source `bdefcade`, the Windows native build and matching guest generation passed. Folder and image-only
runs loaded the same disposable Kokiri checkpoint, displayed generated Xbox prompts and native menu fonts,
and exited successfully without changing the source checkpoint. The image-only run had no loose RomFS root.
A private complete-corpus comparison checked 186,583 files, 598 directories and 26,236,422,768 payload bytes
through the actual guest filesystem implementation against separate native folder reads.

Eighteen focused Windows/Linux jobs passed, including corrupt-input and causal regression controls. Full local
suites had the same failures on the candidate and its exact base `ef81aee7`: Linux 454/1/24 versus 447/1/24,
Windows 442/4/33 versus 435/4/33 (passed/failed/skipped). These are not all-green full-suite results.
Linux source tests do not prove Linux gameplay. The Windows runs cover early gameplay and menus, not the
campaign, deterministic frame equivalence, loading speed or memory savings. The host font callback uses renderer
interface version22, retaining main's screen-size and program-created callbacks. No image/index/game-derived fixture is shipped.

The host selects the virtual font callback only when a packed image is open, before initializing the renderer.
Folder installations keep the original direct-file Reader path, including its input-size behavior. The 64 MiB
compressed-asset limit applies only to the packed virtual reader; its decoded-size limit remains 256 MiB.
Neither the Builder nor launcher selects packed mode automatically. Acceptance of this optional CLI capability
remains a maintainer decision on #1007. Main now uses API21 (including the program-created callback); this proposal uses API22. Later interface changes must rebase and bump again.


Review validation on main `51da0464`, source `329b2a93`: twelve focused checks pass on each OS. The actual renderer
loader selects no virtual font callback in folder mode and the packed callback in image mode. Causal controls
restore the old folder selection or force filesystem probes with a null mod base; both are rejected on both OSes.
The matching Windows native build and current prompt generation pass. Folder and image-only processes load Kokiri,
log FZLTHB merged into the zh-Hans font atlas and exit normally with the source checkpoint preserved. Gameplay
captures are normal; the native menu is absent from those captures, so no new per-glyph/menu visual claim is made.
Generation reuses verified `23c50087` output: all existing nx2 generator modules are identical, with only the new
standalone packer added. Prompt assets are regenerated from this source. Full suites: Windows465/3/35 (the same
three exact-main failures); Linux478/1/24, failing the unchanged control fixture's newest-save ordering assertion.
The original failed run is retained; these are not full-green local suites. Hosted CI is recorded in the PR.

## Prompt resource-table discovery

After main added exact game-table selection for prompt/mod merging, packed mode discovers the first matching base file by name in the immutable image index. It ignores directories, nested files and stale loose roots, and refuses an undersized output buffer. Folder mode retains the existing directory enumeration. This preserves main's first-by-name rule before selecting the matching mod table; it does not introduce a new version-selection rule. The game-free regression runs the actual prompt discovery and complete filesystem backend against authored folders/images, including an image-only root; the old folder-only caller fails the stale-root assertion. The no-mod native runs below do not exercise a mod-supplied resource table.

## Matched Windows runtime validation

Implementation `1cd9b772`, rebased on main `4dd972a8`, was compiled as a matching Windows host/backend pair.
The complete `nx2` and a64-kit generator trees were unchanged from the integrity-checked `3bdde84f` generation;
that verified output and its original provenance were retained. Normal source/header/flag keys reused objects;
no cache guard or lift stamp was overridden. The 16 prompt assets likewise retain their `3bdde84f` producer.

Two fresh scripted processes used identical binaries, assets, options and copies of the same Kokiri child checkpoint:
one with the extracted folder, the other with the packed image and a loose-root path that did not exist. Both rendered
Kokiri and its HUD, opened the native Graphics menu in zh-Hans with readable visible labels/help text, and logged
successful acquisition of the game's FZLTHB font with normal system-font supplementation. The guest inventory also
displayed the selected Xbox LB/RB and Select/Back prompts. Both exited normally; source checkpoint hashes remained
unchanged, as did the packed image's hash.

The output was configured as 3440x1440/21:9, with an observed 1444x720 window client. This is windowed correctness
coverage, not validation of a physical ultrawide display. The native-menu inspection used a window capture:
the control bridge's game-frame PNG does not include the native ImGui menu. Only the visible glyphs were checked,
not the entire CJK repertoire. No Linux/AMD gameplay, mod-table interoperability, full-campaign, FPS, startup-time or
memory-saving result is implied. No game-derived files or private captures are included in the repository.
