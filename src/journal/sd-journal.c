/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>

#include "sd-journal.h"
#include "journal-def.h"
#include "journal-file.h"
#include "hashmap.h"
#include "list.h"
#include "lookup3.h"

typedef struct Match Match;

struct Match {
        char *data;
        size_t size;
        uint64_t le_hash;

        LIST_FIELDS(Match, matches);
};

struct sd_journal {
        Hashmap *files;

        JournalFile *current_file;
        uint64_t current_field;

        LIST_HEAD(Match, matches);
        unsigned n_matches;
};

static void reset_location(sd_journal *j) {
        Iterator i;
        JournalFile *f;

        assert(j);

        j->current_file = NULL;
        j->current_field = 0;

        HASHMAP_FOREACH(f, j->files, i)
                f->current_offset = 0;
}

int sd_journal_add_match(sd_journal *j, const void *data, size_t size) {
        Match *m;

        assert(j);

        if (size <= 0)
                return -EINVAL;

        assert(data);

        m = new0(Match, 1);
        if (!m)
                return -ENOMEM;

        m->size = size;

        m->data = malloc(m->size);
        if (!m->data) {
                free(m);
                return -ENOMEM;
        }

        memcpy(m->data, data, size);
        m->le_hash = hash64(m->data, size);

        LIST_PREPEND(Match, matches, j->matches, m);
        j->n_matches ++;

        reset_location(j);

        return 0;
}

void sd_journal_flush_matches(sd_journal *j) {
        assert(j);

        while (j->matches) {
                Match *m = j->matches;

                LIST_REMOVE(Match, matches, j->matches, m);
                free(m->data);
                free(m);
        }

        j->n_matches = 0;

        reset_location(j);
}

static int compare_order(JournalFile *af, Object *ao, uint64_t ap,
                         JournalFile *bf, Object *bo, uint64_t bp) {

        uint64_t a, b;

        /* We operate on two different files here, hence we can access
         * two objects at the same time, which we normally can't.
         *
         * If contents and timestamps match, these entries are
         * identical, even if the seqnum does not match */

        if (sd_id128_equal(ao->entry.boot_id, bo->entry.boot_id) &&
            ao->entry.monotonic == bo->entry.monotonic &&
            ao->entry.realtime == bo->entry.realtime &&
            ao->entry.xor_hash == bo->entry.xor_hash)
                return 0;

        if (sd_id128_equal(af->header->seqnum_id, bf->header->seqnum_id)) {

                /* If this is from the same seqnum source, compare
                 * seqnums */
                a = le64toh(ao->entry.seqnum);
                b = le64toh(bo->entry.seqnum);

                if (a < b)
                        return -1;
                if (a > b)
                        return 1;

                /* Wow! This is weird, different data but the same
                 * seqnums? Something is borked, but let's make the
                 * best of it and compare by time. */
        }

        if (sd_id128_equal(ao->entry.boot_id, bo->entry.boot_id)) {

                /* If the boot id matches compare monotonic time */
                a = le64toh(ao->entry.monotonic);
                b = le64toh(bo->entry.monotonic);

                if (a < b)
                        return -1;
                if (a > b)
                        return 1;
        }

        /* Otherwise compare UTC time */
        a = le64toh(ao->entry.realtime);
        b = le64toh(ao->entry.realtime);

        if (a < b)
                return -1;
        if (a > b)
                return 1;

        /* Finally, compare by contents */
        a = le64toh(ao->entry.xor_hash);
        b = le64toh(ao->entry.xor_hash);

        if (a < b)
                return -1;
        if (a > b)
                return 1;

        return 0;
}

static int move_to_next_with_matches(sd_journal *j, JournalFile *f, direction_t direction, Object **o, uint64_t *p) {
        int r;
        uint64_t cp;
        Object *c;

        assert(j);
        assert(f);
        assert(o);
        assert(p);

        if (!j->matches) {
                /* No matches is easy, just go on to the next entry */

                if (f->current_offset > 0) {
                        r = journal_file_move_to_object(f, f->current_offset, OBJECT_ENTRY, &c);
                        if (r < 0)
                                return r;
                } else
                        c = NULL;

                return journal_file_next_entry(f, c, direction, o, p);
        }

        /* So there are matches we have to adhere to, let's find the
         * first entry that matches all of them */

        if (f->current_offset > 0)
                cp = f->current_offset;
        else {
                r = journal_file_find_first_entry(f, j->matches->data, j->matches->size, direction, &c, &cp);
                if (r <= 0)
                        return r;

                /* We can shortcut this if there's only one match */
                if (j->n_matches == 1) {
                        *o = c;
                        *p = cp;
                        return r;
                }
        }

        for (;;) {
                uint64_t np, n;
                bool found;
                Match *m;

                r = journal_file_move_to_object(f, cp, OBJECT_ENTRY, &c);
                if (r < 0)
                        return r;

                n = journal_file_entry_n_items(c);

                /* Make sure we don't match the entry we are starting
                 * from. */
                found = f->current_offset != cp;

                np = 0;
                LIST_FOREACH(matches, m, j->matches) {
                        uint64_t q, k;

                        for (k = 0; k < n; k++)
                                if (c->entry.items[k].hash == m->le_hash)
                                        break;

                        if (k >= n) {
                                /* Hmm, didn't find any field that matched, so ignore
                                 * this match. Go on with next match */

                                found = false;
                                continue;
                        }

                        /* Hmm, so, this field matched, let's remember
                         * where we'd have to try next, in case the other
                         * matches are not OK */

                        if (direction == DIRECTION_DOWN) {
                                q = le64toh(c->entry.items[k].next_entry_offset);

                                if (q > np)
                                        np = q;
                        } else {
                                q = le64toh(c->entry.items[k].prev_entry_offset);

                                if (q != 0 && (np == 0 || q < np))
                                        np = q;
                        }
                }

                /* Did this entry match against all matches? */
                if (found) {
                        *o = c;
                        *p = cp;
                        return 1;
                }

                /* Did we find a subsequent entry? */
                if (np == 0)
                        return 0;

                /* Hmm, ok, this entry only matched partially, so
                 * let's try another one */
                cp = np;
        }
}

static int real_journal_next(sd_journal *j, direction_t direction) {
        JournalFile *f, *new_current = NULL;
        Iterator i;
        int r;
        uint64_t new_offset = 0;
        Object *new_entry = NULL;

        assert(j);

        HASHMAP_FOREACH(f, j->files, i) {
                Object *o;
                uint64_t p;

                r = move_to_next_with_matches(j, f, direction, &o, &p);
                if (r < 0)
                        return r;
                else if (r == 0)
                        continue;

                if (!new_current ||
                    compare_order(new_current, new_entry, new_offset, f, o, p) > 0) {
                        new_current = f;
                        new_entry = o;
                        new_offset = p;
                }
        }

        if (new_current) {
                j->current_file = new_current;
                j->current_file->current_offset = new_offset;
                j->current_field = 0;

                /* Skip over any identical entries in the other files too */

                HASHMAP_FOREACH(f, j->files, i) {
                        Object *o;
                        uint64_t p;

                        if (j->current_file == f)
                                continue;

                        r = move_to_next_with_matches(j, f, direction, &o, &p);
                        if (r < 0)
                                return r;
                        else if (r == 0)
                                continue;

                        if (compare_order(new_current, new_entry, new_offset, f, o, p) == 0)
                                f->current_offset = p;
                }

                return 1;
        }

        return 0;
}

int sd_journal_next(sd_journal *j) {
        return real_journal_next(j, DIRECTION_DOWN);
}

int sd_journal_previous(sd_journal *j) {
        return real_journal_next(j, DIRECTION_UP);
}

int sd_journal_get_cursor(sd_journal *j, char **cursor) {
        Object *o;
        int r;
        char bid[33], sid[33];

        assert(j);
        assert(cursor);

        if (!j->current_file || j->current_file->current_offset <= 0)
                return -EADDRNOTAVAIL;

        r = journal_file_move_to_object(j->current_file, j->current_file->current_offset, OBJECT_ENTRY, &o);
        if (r < 0)
                return r;

        sd_id128_to_string(j->current_file->header->seqnum_id, sid);
        sd_id128_to_string(o->entry.boot_id, bid);

        if (asprintf(cursor,
                     "s=%s;i=%llx;b=%s;m=%llx;t=%llx;x=%llx;p=%s",
                     sid, (unsigned long long) le64toh(o->entry.seqnum),
                     bid, (unsigned long long) le64toh(o->entry.monotonic),
                     (unsigned long long) le64toh(o->entry.realtime),
                     (unsigned long long) le64toh(o->entry.xor_hash),
                     file_name_from_path(j->current_file->path)) < 0)
                return -ENOMEM;

        return 1;
}

int sd_journal_set_cursor(sd_journal *j, const char *cursor) {
        return -EINVAL;
}

static int add_file(sd_journal *j, const char *prefix, const char *dir, const char *filename) {
        char *fn;
        int r;
        JournalFile *f;

        assert(j);
        assert(prefix);
        assert(filename);

        if (dir)
                fn = join(prefix, "/", dir, "/", filename, NULL);
        else
                fn = join(prefix, "/", filename, NULL);

        if (!fn)
                return -ENOMEM;

        r = journal_file_open(fn, O_RDONLY, 0, NULL, &f);
        free(fn);

        if (r < 0) {
                if (errno == ENOENT)
                        return 0;

                return r;
        }

        r = hashmap_put(j->files, f->path, f);
        if (r < 0) {
                journal_file_close(f);
                return r;
        }

        return 0;
}

static int add_directory(sd_journal *j, const char *prefix, const char *dir) {
        char *fn;
        int r;
        DIR *d;

        assert(j);
        assert(prefix);
        assert(dir);

        fn = join(prefix, "/", dir, NULL);
        if (!fn)
                return -ENOMEM;

        d = opendir(fn);
        free(fn);

        if (!d) {
                if (errno == ENOENT)
                        return 0;

                return -errno;
        }

        for (;;) {
                struct dirent buf, *de;

                r = readdir_r(d, &buf, &de);
                if (r != 0 || !de)
                        break;

                if (!dirent_is_file_with_suffix(de, ".journal"))
                        continue;

                r = add_file(j, prefix, dir, de->d_name);
                if (r < 0)
                        log_debug("Failed to add file %s/%s/%s: %s", prefix, dir, de->d_name, strerror(-r));
        }

        closedir(d);

        return 0;
}

int sd_journal_open(sd_journal **ret) {
        sd_journal *j;
        const char *p;
        const char search_paths[] =
                "/run/log/journal\0"
                "/var/log/journal\0";
        int r;

        assert(ret);

        j = new0(sd_journal, 1);
        if (!j)
                return -ENOMEM;

        j->files = hashmap_new(string_hash_func, string_compare_func);
        if (!j->files) {
                r = -ENOMEM;
                goto fail;
        }

        /* We ignore most errors here, since the idea is to only open
         * what's actually accessible, and ignore the rest. */

        NULSTR_FOREACH(p, search_paths) {
                DIR *d;

                d = opendir(p);
                if (!d) {
                        if (errno != ENOENT)
                                log_debug("Failed to open %s: %m", p);
                        continue;
                }

                for (;;) {
                        struct dirent buf, *de;
                        sd_id128_t id;

                        r = readdir_r(d, &buf, &de);
                        if (r != 0 || !de)
                                break;

                        if (dirent_is_file_with_suffix(de, ".journal")) {
                                r = add_file(j, p, NULL, de->d_name);
                                if (r < 0)
                                        log_debug("Failed to add file %s/%s: %s", p, de->d_name, strerror(-r));

                        } else if ((de->d_type == DT_DIR || de->d_type == DT_UNKNOWN) &&
                                   sd_id128_from_string(de->d_name, &id) >= 0) {

                                r = add_directory(j, p, de->d_name);
                                if (r < 0)
                                        log_debug("Failed to add directory %s/%s: %s", p, de->d_name, strerror(-r));
                        }
                }

                closedir(d);
        }

        *ret = j;
        return 0;

fail:
        sd_journal_close(j);

        return r;
};

void sd_journal_close(sd_journal *j) {
        assert(j);

        if (j->files) {
                JournalFile *f;

                while ((f = hashmap_steal_first(j->files)))
                        journal_file_close(f);

                hashmap_free(j->files);
        }

        sd_journal_flush_matches(j);

        free(j);
}

int sd_journal_get_realtime_usec(sd_journal *j, uint64_t *ret) {
        Object *o;
        JournalFile *f;
        int r;

        assert(j);
        assert(ret);

        f = j->current_file;
        if (!f)
                return 0;

        if (f->current_offset <= 0)
                return 0;

        r = journal_file_move_to_object(f, f->current_offset, OBJECT_ENTRY, &o);
        if (r < 0)
                return r;

        *ret = le64toh(o->entry.realtime);
        return 1;
}

int sd_journal_get_monotonic_usec(sd_journal *j, uint64_t *ret) {
        Object *o;
        JournalFile *f;
        int r;
        sd_id128_t id;

        assert(j);
        assert(ret);

        f = j->current_file;
        if (!f)
                return 0;

        if (f->current_offset <= 0)
                return 0;

        r = sd_id128_get_boot(&id);
        if (r < 0)
                return r;

        r = journal_file_move_to_object(f, f->current_offset, OBJECT_ENTRY, &o);
        if (r < 0)
                return r;

        if (!sd_id128_equal(id, o->entry.boot_id))
                return 0;

        *ret = le64toh(o->entry.monotonic);
        return 1;

}

int sd_journal_get_data(sd_journal *j, const char *field, const void **data, size_t *size) {
        JournalFile *f;
        uint64_t i, n;
        size_t field_length;
        int r;
        Object *o;

        assert(j);
        assert(field);
        assert(data);
        assert(size);

        if (isempty(field) || strchr(field, '='))
                return -EINVAL;

        f = j->current_file;
        if (!f)
                return 0;

        if (f->current_offset <= 0)
                return 0;

        r = journal_file_move_to_object(f, f->current_offset, OBJECT_ENTRY, &o);
        if (r < 0)
                return r;

        field_length = strlen(field);

        n = journal_file_entry_n_items(o);
        for (i = 0; i < n; i++) {
                uint64_t p, l, h;
                size_t t;

                p = le64toh(o->entry.items[i].object_offset);
                h = o->entry.items[j->current_field].hash;
                r = journal_file_move_to_object(f, p, OBJECT_DATA, &o);
                if (r < 0)
                        return r;

                if (h != o->data.hash)
                        return -EBADMSG;

                l = le64toh(o->object.size) - offsetof(Object, data.payload);

                if (l >= field_length+1 &&
                    memcmp(o->data.payload, field, field_length) == 0 &&
                    o->data.payload[field_length] == '=') {

                        t = (size_t) l;

                        if ((uint64_t) t != l)
                                return -E2BIG;

                        *data = o->data.payload;
                        *size = t;

                        return 1;
                }

                r = journal_file_move_to_object(f, f->current_offset, OBJECT_ENTRY, &o);
                if (r < 0)
                        return r;
        }

        return 0;
}

int sd_journal_enumerate_data(sd_journal *j, const void **data, size_t *size) {
        JournalFile *f;
        uint64_t p, l, n, h;
        size_t t;
        int r;
        Object *o;

        assert(j);
        assert(data);
        assert(size);

        f = j->current_file;
        if (!f)
                return 0;

        if (f->current_offset <= 0)
                return 0;

        r = journal_file_move_to_object(f, f->current_offset, OBJECT_ENTRY, &o);
        if (r < 0)
                return r;

        n = journal_file_entry_n_items(o);
        if (j->current_field >= n)
                return 0;

        p = le64toh(o->entry.items[j->current_field].object_offset);
        h = o->entry.items[j->current_field].hash;
        r = journal_file_move_to_object(f, p, OBJECT_DATA, &o);
        if (r < 0)
                return r;

        if (h != o->data.hash)
                return -EBADMSG;

        l = le64toh(o->object.size) - offsetof(Object, data.payload);
        t = (size_t) l;

        /* We can't read objects larger than 4G on a 32bit machine */
        if ((uint64_t) t != l)
                return -E2BIG;

        *data = o->data.payload;
        *size = t;

        j->current_field ++;

        return 1;
}

void sd_journal_start_data(sd_journal *j) {
        assert(j);

        j->current_field = 0;
}

int sd_journal_seek_head(sd_journal *j) {
        assert(j);

        reset_location(j);

        return real_journal_next(j, DIRECTION_DOWN);
}

int sd_journal_seek_tail(sd_journal *j) {
        assert(j);

        reset_location(j);

        return real_journal_next(j, DIRECTION_UP);
}
