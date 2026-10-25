# grub-wrap

[A GRUB 2 module](wrap.c) that attaches files to a root task and hands the whole thing to the kernel as a single Multiboot2 module. 
[A one-header C](pool.h) reader is given too for the root task to read the attached files.

## Why this exists

`wrap` was written for an operating system built on the seL4 kernel (v16.0.0), on x86_64, booted with GRUB (v2.16). With that setup the kernel uses only the first Multiboot2 module (the root task) and does not expose the others to it, so there is no direct way to hand several files to the root task.

The usual answer in the seL4 ecosystem is a CPIO archive: the files are packed into an archive at build time, and the archive is linked into the root task. That works, but the files then live inside a build artifact. Changing one of them means rebuilding the archive and the root task, and the files are no longer ordinary files on the disk.

The goal here was the opposite: the files stay as plain files on the boot partition, and are picked up as they are at every boot. GRUB already knows how to read the disk and its filesystem before the kernel starts, so the module takes advantage of that. The root task receives its resources without needing a storage driver, and can then use them to bring up its own drivers and mount the real filesystem.

## Usage
`grub.cfg:`
```
multiboot2 kernel.elf
wrap roottask.elf FILE1 FILE2 ...
boot
```

- `wrap` must run after `multiboot2` (it uses the loader's relocator) and replaces `module2` for the root task.
- The first argument is the root task ELF. Every further argument is a file to attach. Paths are resolved by GRUB, so `(hd0,gpt1)/x` works.
- File names are stored exactly as typed and are the keys the root task looks up.

`roottask:`
```c
#include "pool.h"

PoolInfo pool = get_pool_info();
if (!pool.ok) {
    printf("pool error: %s\n", pool.error);
} else {
    for (int i = 0; i < pool.nb; i++)
        printf("%s: %u bytes at %p\n", pool.name[i],
               (unsigned) pool.size[i], (const void *) pool.start[i]);
}
```

`start[i]` points at the first byte of file `name[i]`; its contents are `size[i]` bytes long. Names and contents live inside the pool: nothing is copied or allocated.
If an error occurs, pool.ok switches to false and pool.error contains the error message.

## How it works
### Wrap header
`wrap` builds one block in memory:

```
offset 0      wrapper ELF header + program headers   (one page, zero padded)
offset 4096   original root task ELF, byte for byte
offset x      pool: header, table of contents, file contents
              (x = end of the original ELF, rounded up to a page)
```

The kernel only reads program headers. The wrapper header is a copy of the original ELF header whose program header table lists every original segment (file offsets shifted by 4096; virtual addresses and sizes unchanged) followed by one extra read-only `PT_LOAD` for the pool. The pool is mapped at the first page boundary above the highest original segment, which is `ALIGN_UP(&_end, 4096)` from the root task's point of view. The root task's own headers, as it sees them in memory, are the original ones: it is unaware of the wrapper.

### Pool
Here is the actual pool format:

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | signature `POOL` |
| 4 | 1 | version, `1` |
| 5 | 1 | `w`: width in bytes of a file size, 1 to 4 |
| 6 | 1 | `nb`: number of files, 0 to 64 |
| 7 | | Table of contents, with `nb` entries |
| | | The file contents |

Each table entry is the file size on `w` bytes, **big-endian**, followed by the file name and a terminating `0` byte.
Contents follow the table, concatenated in table order, with no padding. `w` is the smallest width that holds the largest file.

## Building the module
1. Get the [grub source code](https://gitlab.freedesktop.org/gnu-grub/grub/) and compile it completely. (WARNING: GRUB compilation can be really long).
2. Download and extract a version of grub-wrap.
3. Run `make link DIR=/path/to/your/compiled/grub/directory`.
4. If everything went right, run `make build DIR=/path/to/your/compiled/grub/directory`.
5. The module has now been added as wrap. You can use `grub-mkimage` and include it as every other module to create a GRUB bootable image.

If needed, you can unlink and delete the module from the grub installation with `make unlink DIR=/path/to/your/compiled/grub/directory`

WARNING: `wrap.mod` must be built from the same GRUB version, for the same platform, as the GRUB that loads it. It is not a standalone binary.

## Notes
- Limits: 64 files, 4 GiB - 1 bytes per file, 255 bytes per name.
- The module's memory is allocated through the Multiboot relocator, below 4 GiB, page aligned. A block much larger than the kernel's gaps also avoids being placed in a small hole between kernel sections. If the kernel refuses to boot because the module was placed in a small gap, pad the last attached file with zeros (a few MiB is enough).
- `wrap` reads files with compression filters disabled, so a `.gz` file is attached as-is.
- On any error `wrap` returns a GRUB error and registers no module; boot then stops at the `boot` command.

## License

`wrap.c` is distributed (as every GRUB module) under the [GNU General Public License](LICENSES/GPL-3.0-or-later.txt), version 3 or any later version. If you distribute a GRUB image containing `wrap.mod`, provide the corresponding source of the module along with it.

`pool.h`, `Makefile`, `Makefile.core.def` and `README.MD` are released under the [MIT license](LICENSES/MIT.txt). The copyright notice must be kept in copies and derived works.

The full license texts are in the `LICENSES/` directory, named after their SPDX identifiers.

Copyright (c) 2026 End0832_.
SPDX-License-Identifier: MIT (text in LICENSES/MIT.txt)
