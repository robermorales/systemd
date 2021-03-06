/***
  This file is part of systemd.

  Copyright 2017 Zbigniew Jędrzejewski-Szmek

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdio.h>
#include <linux/magic.h>

#include "alloc-util.h"
#include "blkid-util.h"
#include "bootspec.h"
#include "conf-files.h"
#include "def.h"
#include "efivars.h"
#include "fd-util.h"
#include "fileio.h"
#include "parse-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "virt.h"

void boot_entry_free(BootEntry *entry) {
        free(entry->filename);

        free(entry->title);
        free(entry->show_title);
        free(entry->version);
        free(entry->machine_id);
        free(entry->architecture);
        strv_free(entry->options);
        free(entry->kernel);
        free(entry->efi);
        strv_free(entry->initrd);
        free(entry->device_tree);
}

int boot_entry_load(const char *path, BootEntry *entry) {
        _cleanup_fclose_ FILE *f = NULL;
        unsigned line = 1;
        _cleanup_(boot_entry_free) BootEntry tmp = {};
        int r;

        f = fopen(path, "re");
        if (!f)
                return log_error_errno(errno, "Failed to open \"%s\": %m", path);

        tmp.filename = strdup(basename(path));
        if (!tmp.filename)
                return log_oom();

        for (;;) {
                _cleanup_free_ char *buf = NULL;
                char *p;

                r = read_line(f, LONG_LINE_MAX, &buf);
                if (r == 0)
                        break;
                if (r == -ENOBUFS)
                        return log_error_errno(r, "%s:%u: Line too long", path, line);
                if (r < 0)
                        return log_error_errno(r, "%s:%u: Error while reading: %m", path, line);

                line++;

                if (IN_SET(*strstrip(buf), '#', '\0'))
                        continue;

                p = strchr(buf, ' ');
                if (!p) {
                        log_warning("%s:%u: Bad syntax", path, line);
                        continue;
                }
                *p = '\0';
                p = strstrip(p + 1);

                if (streq(buf, "title"))
                        r = free_and_strdup(&tmp.title, p);
                else if (streq(buf, "version"))
                        r = free_and_strdup(&tmp.version, p);
                else if (streq(buf, "machine-id"))
                        r = free_and_strdup(&tmp.machine_id, p);
                else if (streq(buf, "architecture"))
                        r = free_and_strdup(&tmp.architecture, p);
                else if (streq(buf, "options"))
                        r = strv_extend(&tmp.options, p);
                else if (streq(buf, "linux"))
                        r = free_and_strdup(&tmp.kernel, p);
                else if (streq(buf, "efi"))
                        r = free_and_strdup(&tmp.efi, p);
                else if (streq(buf, "initrd"))
                        r = strv_extend(&tmp.initrd, p);
                else if (streq(buf, "devicetree"))
                        r = free_and_strdup(&tmp.device_tree, p);
                else {
                        log_notice("%s:%u: Unknown line \"%s\"", path, line, buf);
                        continue;
                }
                if (r < 0)
                        return log_error_errno(r, "%s:%u: Error while reading: %m", path, line);
        }

        *entry = tmp;
        tmp = (BootEntry) {};
        return 0;
}

void boot_config_free(BootConfig *config) {
        unsigned i;

        free(config->default_pattern);
        free(config->timeout);
        free(config->editor);

        free(config->entry_oneshot);
        free(config->entry_default);

        for (i = 0; i < config->n_entries; i++)
                boot_entry_free(config->entries + i);
        free(config->entries);
}

int boot_loader_read_conf(const char *path, BootConfig *config) {
        _cleanup_fclose_ FILE *f = NULL;
        unsigned line = 1;
        int r;

        f = fopen(path, "re");
        if (!f)
                return log_error_errno(errno, "Failed to open \"%s\": %m", path);

        for (;;) {
                _cleanup_free_ char *buf = NULL;
                char *p;

                r = read_line(f, LONG_LINE_MAX, &buf);
                if (r == 0)
                        break;
                if (r == -ENOBUFS)
                        return log_error_errno(r, "%s:%u: Line too long", path, line);
                if (r < 0)
                        return log_error_errno(r, "%s:%u: Error while reading: %m", path, line);

                line++;

                if (IN_SET(*strstrip(buf), '#', '\0'))
                        continue;

                p = strchr(buf, ' ');
                if (!p) {
                        log_warning("%s:%u: Bad syntax", path, line);
                        continue;
                }
                *p = '\0';
                p = strstrip(p + 1);

                if (streq(buf, "default"))
                        r = free_and_strdup(&config->default_pattern, p);
                else if (streq(buf, "timeout"))
                        r = free_and_strdup(&config->timeout, p);
                else if (streq(buf, "editor"))
                        r = free_and_strdup(&config->editor, p);
                else {
                        log_notice("%s:%u: Unknown line \"%s\"", path, line, buf);
                        continue;
                }
                if (r < 0)
                        return log_error_errno(r, "%s:%u: Error while reading: %m", path, line);
        }

        return 0;
}

/* This is a direct translation of str_verscmp from boot.c */
static bool is_digit(int c) {
        return c >= '0' && c <= '9';
}

static int c_order(int c) {
        if (c == '\0')
                return 0;
        if (is_digit(c))
                return 0;
        else if ((c >= 'a') && (c <= 'z'))
                return c;
        else
                return c + 0x10000;
}

static int str_verscmp(const char *s1, const char *s2) {
        const char *os1 = s1;
        const char *os2 = s2;

        while (*s1 || *s2) {
                int first;

                while ((*s1 && !is_digit(*s1)) || (*s2 && !is_digit(*s2))) {
                        int order;

                        order = c_order(*s1) - c_order(*s2);
                        if (order)
                                return order;
                        s1++;
                        s2++;
                }

                while (*s1 == '0')
                        s1++;
                while (*s2 == '0')
                        s2++;

                first = 0;
                while (is_digit(*s1) && is_digit(*s2)) {
                        if (first == 0)
                                first = *s1 - *s2;
                        s1++;
                        s2++;
                }

                if (is_digit(*s1))
                        return 1;
                if (is_digit(*s2))
                        return -1;

                if (first != 0)
                        return first;
        }

        return strcmp(os1, os2);
}

static int boot_entry_compare(const void *a, const void *b) {
        const BootEntry *aa = a;
        const BootEntry *bb = b;

        return str_verscmp(aa->filename, bb->filename);
}

int boot_entries_find(const char *dir, BootEntry **entries, size_t *n_entries) {
        _cleanup_strv_free_ char **files = NULL;
        char **f;
        int r;

        BootEntry *array = NULL;
        size_t n_allocated = 0, n = 0;

        r = conf_files_list(&files, ".conf", NULL, 0, dir, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to list files in \"%s\": %m", dir);

        STRV_FOREACH(f, files) {
                if (!GREEDY_REALLOC0(array, n_allocated, n + 1))
                        return log_oom();

                r = boot_entry_load(*f, array + n);
                if (r < 0)
                        continue;

                n++;
        }

        qsort_safe(array, n, sizeof(BootEntry), boot_entry_compare);

        *entries = array;
        *n_entries = n;
        return 0;
}

static bool find_nonunique(BootEntry *entries, size_t n_entries, bool *arr) {
        unsigned i, j;
        bool non_unique = false;

        for (i = 0; i < n_entries; i++)
                arr[i] = false;

        for (i = 0; i < n_entries; i++)
                for (j = 0; j < n_entries; j++)
                        if (i != j && streq(boot_entry_title(entries + i),
                                            boot_entry_title(entries + j)))
                                non_unique = arr[i] = arr[j] = true;

        return non_unique;
}

static int boot_entries_uniquify(BootEntry *entries, size_t n_entries) {
        char *s;
        unsigned i;
        int r;
        bool arr[n_entries];

        /* Find _all_ non-unique titles */
        if (!find_nonunique(entries, n_entries, arr))
                return 0;

        /* Add version to non-unique titles */
        for (i = 0; i < n_entries; i++)
                if (arr[i] && entries[i].version) {
                        r = asprintf(&s, "%s (%s)", boot_entry_title(entries + i), entries[i].version);
                        if (r < 0)
                                return -ENOMEM;

                        free_and_replace(entries[i].show_title, s);
                }

        if (!find_nonunique(entries, n_entries, arr))
                return 0;

        /* Add machine-id to non-unique titles */
        for (i = 0; i < n_entries; i++)
                if (arr[i] && entries[i].machine_id) {
                        r = asprintf(&s, "%s (%s)", boot_entry_title(entries + i), entries[i].machine_id);
                        if (r < 0)
                                return -ENOMEM;

                        free_and_replace(entries[i].show_title, s);
                }

        if (!find_nonunique(entries, n_entries, arr))
                return 0;

        /* Add file name to non-unique titles */
        for (i = 0; i < n_entries; i++)
                if (arr[i]) {
                        r = asprintf(&s, "%s (%s)", boot_entry_title(entries + i), entries[i].filename);
                        if (r < 0)
                                return -ENOMEM;

                        free_and_replace(entries[i].show_title, s);
                }

        return 0;
}

int boot_entries_select_default(const BootConfig *config) {
        int i;

        if (config->entry_oneshot)
                for (i = config->n_entries - 1; i >= 0; i--)
                        if (streq(config->entry_oneshot, config->entries[i].filename)) {
                                log_debug("Found default: filename \"%s\" is matched by LoaderEntryOneShot",
                                          config->entries[i].filename);
                                return i;
                        }

        if (config->entry_default)
                for (i = config->n_entries - 1; i >= 0; i--)
                        if (streq(config->entry_default, config->entries[i].filename)) {
                                log_debug("Found default: filename \"%s\" is matched by LoaderEntryDefault",
                                          config->entries[i].filename);
                                return i;
                        }

        if (config->default_pattern)
                for (i = config->n_entries - 1; i >= 0; i--)
                        if (fnmatch(config->default_pattern, config->entries[i].filename, FNM_CASEFOLD) == 0) {
                                log_debug("Found default: filename \"%s\" is matched by pattern \"%s\"",
                                          config->entries[i].filename, config->default_pattern);
                                return i;
                        }

        if (config->n_entries > 0)
                log_debug("Found default: last entry \"%s\"", config->entries[i].filename);
        else
                log_debug("Found no default boot entry :(");
        return config->n_entries - 1; /* -1 means "no default" */
}

int boot_entries_load_config(const char *esp_path, BootConfig *config) {
        const char *p;
        int r;

        p = strjoina(esp_path, "/loader/loader.conf");
        r = boot_loader_read_conf(p, config);
        if (r < 0)
                return log_error_errno(r, "Failed to read boot config from \"%s\": %m", p);

        p = strjoina(esp_path, "/loader/entries");
        r = boot_entries_find(p, &config->entries, &config->n_entries);
        if (r < 0)
                return log_error_errno(r, "Failed to read boot entries from \"%s\": %m", p);

        r = boot_entries_uniquify(config->entries, config->n_entries);
        if (r < 0)
                return log_error_errno(r, "Failed to uniquify boot entries: %m");

        r = efi_get_variable_string(EFI_VENDOR_LOADER, "LoaderEntryOneShot", &config->entry_oneshot);
        if (r < 0 && r != -ENOENT)
                return log_error_errno(r, "Failed to read EFI var \"LoaderEntryOneShot\": %m");

        r = efi_get_variable_string(EFI_VENDOR_LOADER, "LoaderEntryDefault", &config->entry_default);
        if (r < 0 && r != -ENOENT)
                return log_error_errno(r, "Failed to read EFI var \"LoaderEntryDefault\": %m");

        config->default_entry = boot_entries_select_default(config);
        return 0;
}

/********************************************************************************/

static int verify_esp(
                bool searching,
                const char *p,
                uint32_t *ret_part,
                uint64_t *ret_pstart,
                uint64_t *ret_psize,
                sd_id128_t *ret_uuid) {

        _cleanup_blkid_free_probe_ blkid_probe b = NULL;
        _cleanup_free_ char *t = NULL;
        uint64_t pstart = 0, psize = 0;
        struct stat st, st2;
        const char *v, *t2;
        struct statfs sfs;
        sd_id128_t uuid = SD_ID128_NULL;
        uint32_t part = 0;
        bool quiet;
        int r;

        assert(p);

        /* Non-root user can only check the status, so if an error occured in the following,
         * it does not cause any issues. Let's silence the error messages. */
        quiet = geteuid() != 0;

        if (statfs(p, &sfs) < 0) {
                /* If we are searching for the mount point, don't generate a log message if we can't find the path */
                if (errno == ENOENT && searching)
                        return -ENOENT;

                return log_full_errno(quiet && errno == EACCES ? LOG_DEBUG : LOG_ERR, errno,
                                      "Failed to check file system type of \"%s\": %m", p);
        }

        if (!F_TYPE_EQUAL(sfs.f_type, MSDOS_SUPER_MAGIC)) {
                if (searching)
                        return -EADDRNOTAVAIL;

                log_error("File system \"%s\" is not a FAT EFI System Partition (ESP) file system.", p);
                return -ENODEV;
        }

        if (stat(p, &st) < 0)
                return log_full_errno(quiet && errno == EACCES ? LOG_DEBUG : LOG_ERR, errno,
                                      "Failed to determine block device node of \"%s\": %m", p);

        if (major(st.st_dev) == 0) {
                log_error("Block device node of %p is invalid.", p);
                return -ENODEV;
        }

        t2 = strjoina(p, "/..");
        r = stat(t2, &st2);
        if (r < 0)
                return log_full_errno(quiet && errno == EACCES ? LOG_DEBUG : LOG_ERR, errno,
                                      "Failed to determine block device node of parent of \"%s\": %m", p);

        if (st.st_dev == st2.st_dev) {
                log_error("Directory \"%s\" is not the root of the EFI System Partition (ESP) file system.", p);
                return -ENODEV;
        }

        /* In a container we don't have access to block devices, skip this part of the verification, we trust the
         * container manager set everything up correctly on its own. Also skip the following verification for non-root user. */
        if (detect_container() > 0 || geteuid() != 0)
                goto finish;

        r = asprintf(&t, "/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
        if (r < 0)
                return log_oom();

        errno = 0;
        b = blkid_new_probe_from_filename(t);
        if (!b)
                return log_error_errno(errno ?: ENOMEM, "Failed to open file system \"%s\": %m", p);

        blkid_probe_enable_superblocks(b, 1);
        blkid_probe_set_superblocks_flags(b, BLKID_SUBLKS_TYPE);
        blkid_probe_enable_partitions(b, 1);
        blkid_probe_set_partitions_flags(b, BLKID_PARTS_ENTRY_DETAILS);

        errno = 0;
        r = blkid_do_safeprobe(b);
        if (r == -2) {
                log_error("File system \"%s\" is ambiguous.", p);
                return -ENODEV;
        } else if (r == 1) {
                log_error("File system \"%s\" does not contain a label.", p);
                return -ENODEV;
        } else if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe file system \"%s\": %m", p);

        errno = 0;
        r = blkid_probe_lookup_value(b, "TYPE", &v, NULL);
        if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe file system type \"%s\": %m", p);
        if (!streq(v, "vfat")) {
                log_error("File system \"%s\" is not FAT.", p);
                return -ENODEV;
        }

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_SCHEME", &v, NULL);
        if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe partition scheme \"%s\": %m", p);
        if (!streq(v, "gpt")) {
                log_error("File system \"%s\" is not on a GPT partition table.", p);
                return -ENODEV;
        }

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_TYPE", &v, NULL);
        if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe partition type UUID \"%s\": %m", p);
        if (!streq(v, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b")) {
                log_error("File system \"%s\" has wrong type for an EFI System Partition (ESP).", p);
                return -ENODEV;
        }

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_UUID", &v, NULL);
        if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe partition entry UUID \"%s\": %m", p);
        r = sd_id128_from_string(v, &uuid);
        if (r < 0) {
                log_error("Partition \"%s\" has invalid UUID \"%s\".", p, v);
                return -EIO;
        }

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_NUMBER", &v, NULL);
        if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe partition number \"%s\": m", p);
        r = safe_atou32(v, &part);
        if (r < 0)
                return log_error_errno(r, "Failed to parse PART_ENTRY_NUMBER field.");

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_OFFSET", &v, NULL);
        if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe partition offset \"%s\": %m", p);
        r = safe_atou64(v, &pstart);
        if (r < 0)
                return log_error_errno(r, "Failed to parse PART_ENTRY_OFFSET field.");

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_SIZE", &v, NULL);
        if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe partition size \"%s\": %m", p);
        r = safe_atou64(v, &psize);
        if (r < 0)
                return log_error_errno(r, "Failed to parse PART_ENTRY_SIZE field.");

finish:
        if (ret_part)
                *ret_part = part;
        if (ret_pstart)
                *ret_pstart = pstart;
        if (ret_psize)
                *ret_psize = psize;
        if (ret_uuid)
                *ret_uuid = uuid;

        return 0;
}

int find_esp(char **path,
             uint32_t *part, uint64_t *pstart, uint64_t *psize, sd_id128_t *uuid) {

        const char *p;
        int r;

        if (*path)
                return verify_esp(false, *path, part, pstart, psize, uuid);

        FOREACH_STRING(p, "/efi", "/boot", "/boot/efi") {

                r = verify_esp(true, p, part, pstart, psize, uuid);
                if (IN_SET(r, -ENOENT, -EADDRNOTAVAIL)) /* This one is not it */
                        continue;
                if (r < 0)
                        return r;

                *path = strdup(p);
                if (!*path)
                        return log_oom();

                return 0;
        }

        return -ENOENT;
}
