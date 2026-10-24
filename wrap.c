/*
 * wrap.c - GRUB module "wrap"
 * (https://github.com/end0832/grub-wrap)
 *
 * Loads a root task ELF together with a set of attached files as a single
 * Multiboot2 module. Thought for seL4 microkernel (v16.0.0) as it only consumes 
 * the first module and map nothing but the PT_LOAD segments of its ELF image.
 *
 * Usage (grub.cfg):
 *
 *     multiboot2 /kernel.elf
 *     wrap /roottask.elf /path/to/a.txt /path/to/b.bin
 *     boot
 *
 * For more informations, please check the README.MD file.
 *
 * Copyright (C) 2026 End0832_
 * SPDX-License-Identifier: GPL-3.0-or-later (text in LICENSES/GPL-3.0-or-later.txt)
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/dl.h>
#include <grub/command.h>
#include <grub/file.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/err.h>
#include <grub/i18n.h>
#include <grub/elf.h>
#include <grub/relocator.h>
#include <grub/multiboot2.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define WRAP_MAX_FILES    64      /* limit of the pool format */
#define WRAP_NAME_MAX     255     /* longest file name, without the NUL */
#define WRAP_PAGE         4096
#define WRAP_POOL_VERSION 1
#define WRAP_PF_R         4       /* pool segment is read-only */

/* Store the n low-order bytes of v at p, most significant byte first. */
static void
put_be (unsigned char *p, grub_uint64_t v, int n)
{
  while (n--)
    {
      p[n] = v & 0xff;
      v >>= 8;
    }
}

/* Return the size of a file without reading it.  Decompression is disabled so
   that the raw bytes are measured, as with module2 --nounzip. */
static grub_err_t
file_size (const char *path, grub_uint64_t *out)
{
  grub_file_t f;

  *out = 0;
  f = grub_file_open (path, GRUB_FILE_TYPE_MULTIBOOT_MODULE
                            | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!f)
    return grub_errno;
  *out = f->size;
  grub_file_close (f);
  return GRUB_ERR_NONE;
}

/* Read exactly size bytes of a file straight into dst. */
static grub_err_t
file_load (const char *path, void *dst, grub_uint64_t size)
{
  grub_file_t f;

  f = grub_file_open (path, GRUB_FILE_TYPE_MULTIBOOT_MODULE
                            | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (!f)
    return grub_errno;
  if (grub_file_read (f, dst, size) != (grub_ssize_t) size)
    {
      grub_file_close (f);
      if (!grub_errno)
        grub_error (GRUB_ERR_FILE_READ_ERROR, N_("premature end of file %s"),
                    path);
      return grub_errno;
    }
  grub_file_close (f);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_wrap (grub_command_t cmd __attribute__ ((unused)),
               int argc, char *argv[])
{
  /* argv[0] is the root task ELF; argv[1..] are the pool files. */
  grub_uint64_t size[WRAP_MAX_FILES];
  grub_uint64_t elf_size, max_size = 0, toc = 0, sum = 0;
  grub_uint64_t pool_len, X, D, total, top = 0, V;
  grub_relocator_chunk_t ch;
  unsigned char *out, *pool, *p;
  Elf64_Ehdr *old, *eh;
  Elf64_Phdr *oph, *nph;
  int nfiles, nload = 0, n, i, ps;
  grub_err_t err;

  if (argc < 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       N_("usage: wrap ROOTTASK_ELF [FILES...]"));

  nfiles = argc - 1;
  if (nfiles > WRAP_MAX_FILES)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("too many files (max %d)"),
                       WRAP_MAX_FILES);

  /* The relocator is created by the multiboot2 command. */
  if (!grub_multiboot2_relocator)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       N_("you need to load the kernel first"));

  /* Measure everything first: the whole layout is known before any write. */
  err = file_size (argv[0], &elf_size);
  if (err)
    return err;

  for (i = 0; i < nfiles; i++)
    {
      err = file_size (argv[i + 1], &size[i]);
      if (err)
        return err;
      if (size[i] > 0xffffffffULL)
        return grub_error (GRUB_ERR_BAD_ARGUMENT,
                           N_("%s is too big (max 4 GiB - 1)"), argv[i + 1]);
      if (grub_strlen (argv[i + 1]) > WRAP_NAME_MAX)
        return grub_error (GRUB_ERR_BAD_ARGUMENT,
                           N_("file name too long (max %d): %s"),
                           WRAP_NAME_MAX, argv[i + 1]);
      if (size[i] > max_size)
        max_size = size[i];
      sum += size[i];
      toc += grub_strlen (argv[i + 1]) + 1;           /* name + NUL */
    }

  /* Smallest size width, in bytes, able to hold the largest file. */
  for (ps = 1; ps < 4 && (max_size >> (8 * ps)); ps++)
    ;
  toc += (grub_uint64_t) nfiles * ps;

  /* Layout. */
  pool_len = 7 + toc + sum;                   /* 7 = signature + 3 header bytes */
  X = WRAP_PAGE;                              /* original ELF offset */
  D = ALIGN_UP (X + elf_size, WRAP_PAGE);     /* pool segment offset */
  total = D + pool_len;
  if (total > 0xffffffffULL)
    return grub_error (GRUB_ERR_OUT_OF_RANGE,
                       N_("module too big for Multiboot"));

  /* One page-aligned block below 4 GiB.  The relocator owns it from here on
     and releases it together with the loaded kernel. */
  err = grub_relocator_alloc_chunk_align (grub_multiboot2_relocator, &ch,
                                          0, UP_TO_TOP32 (total),
                                          total, WRAP_PAGE,
                                          GRUB_RELOCATOR_PREFERENCE_NONE, 1);
  if (err)
    return err;
  out = get_virtual_current_address (ch);
  grub_memset (out, 0, total);

  /* Read the original ELF in place, then validate it. */
  err = file_load (argv[0], out + X, elf_size);
  if (err)
    return err;

  old = (Elf64_Ehdr *) (out + X);
  if (elf_size < sizeof (Elf64_Ehdr)
      || grub_memcmp (old->e_ident, ELFMAG, SELFMAG) != 0
      || old->e_ident[EI_CLASS] != ELFCLASS64
      || old->e_ident[EI_DATA] != ELFDATA2LSB
      || old->e_phentsize != sizeof (Elf64_Phdr)
      || old->e_phoff + (grub_uint64_t) old->e_phnum * sizeof (Elf64_Phdr)
         > elf_size)
    return grub_error (GRUB_ERR_BAD_OS, N_("%s is not a valid x86_64 ELF"),
                       argv[0]);

  n = old->e_phnum;
  if (sizeof (Elf64_Ehdr) + (n + 1) * sizeof (Elf64_Phdr) > WRAP_PAGE)
    return grub_error (GRUB_ERR_BAD_OS, N_("too many program headers"));

  oph = (Elf64_Phdr *) (out + X + old->e_phoff);

  /* The pool goes at the first page boundary above the highest PT_LOAD. */
  for (i = 0; i < n; i++)
    if (oph[i].p_type == PT_LOAD)
      {
        nload++;
        if (oph[i].p_vaddr + oph[i].p_memsz > top)
          top = oph[i].p_vaddr + oph[i].p_memsz;
      }
  if (!nload)
    return grub_error (GRUB_ERR_BAD_OS, N_("%s has no PT_LOAD segment"),
                       argv[0]);
  V = ALIGN_UP (top, WRAP_PAGE);

  /* Wrapper ELF header: a copy of the original, with the program header table
     relocated right after it and the section table dropped (its offsets would
     be stale after the shift). */
  eh = (Elf64_Ehdr *) out;
  nph = (Elf64_Phdr *) (out + sizeof (Elf64_Ehdr));

  grub_memcpy (eh, old, sizeof (Elf64_Ehdr));
  eh->e_phoff = sizeof (Elf64_Ehdr);
  eh->e_phnum = n + 1;
  eh->e_shoff = 0;
  eh->e_shnum = 0;
  eh->e_shstrndx = 0;

  /* Original program headers: only the file offset changes, by X.  Virtual
     addresses and sizes are untouched.  PT_PHDR described the old table. */
  for (i = 0; i < n; i++)
    {
      nph[i] = oph[i];
      nph[i].p_offset += X;
      if (nph[i].p_type == PT_PHDR)
        nph[i].p_type = PT_NULL;
    }

  /* Pool segment. */
  nph[n].p_type = PT_LOAD;
  nph[n].p_flags = WRAP_PF_R;
  nph[n].p_offset = D;
  nph[n].p_vaddr = V;
  nph[n].p_paddr = V;
  nph[n].p_filesz = pool_len;
  nph[n].p_memsz = pool_len;
  nph[n].p_align = WRAP_PAGE;

  /* Pool header. */
  pool = out + D;
  p = pool + 7;
  grub_memcpy (pool, "POOL", 4);
  pool[4] = WRAP_POOL_VERSION;
  pool[5] = ps;
  pool[6] = nfiles;

  /* Table of contents: size, then name. */
  for (i = 0; i < nfiles; i++)
    {
      grub_size_t len = grub_strlen (argv[i + 1]) + 1;

      put_be (p, size[i], ps);
      p += ps;
      grub_memcpy (p, argv[i + 1], len);
      p += len;
    }

  /* File contents, read directly to their final location. */
  for (i = 0; i < nfiles; i++)
    {
      err = file_load (argv[i + 1], p, size[i]);
      if (err)
        return err;
      p += size[i];
    }

  if ((grub_uint64_t) (p - pool) != pool_len)
    return grub_error (GRUB_ERR_BUG, "wrap: internal size mismatch");

  /* Register the whole block as one Multiboot2 module; its command line is
     the name of the root task ELF. */
  return grub_multiboot2_add_module (get_physical_target_address (ch), total,
                                     1, argv);
}

static grub_command_t cmd;

GRUB_MOD_INIT (wrap)
{
  cmd = grub_register_command ("wrap", grub_cmd_wrap,
                               N_("ROOTTASK_ELF [FILES...]"),
                               N_("Load a root task ELF with attached files "
                                  "as a single Multiboot2 module."));
}

GRUB_MOD_FINI (wrap)
{
  grub_unregister_command (cmd);
}