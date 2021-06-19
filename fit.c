/*
 * Copyright (c) 2020 Axel Scheepers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

static const char *const g_fit_usage_string = "\
usage:  fit -s size [-l destdir] [-nr] path [path ...]\n\
\n\
options:\n\
  -s size    disk size in k, m, g, or t.\n\
  -l destdir directory to link files into,\n\
             if omitted just print the disks.\n\
  -n         show the number of disks it takes.\n\
  -r         recursive search of the path.\n\
  path       path to the files to fit.\n\
\n";

#define _DEFAULT_SOURCE

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <err.h>
#include <unistd.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB 1000L
#define MB (KB * KB)
#define GB (MB * KB)
#define TB (GB * KB)

#define BUFSIZE 512

static off_t g_disk_size = 0;

enum { false, true };

/*
 * The next couple of functions are wrappers around their
 * c stdlib/posix counterparts which exit on error.
 */
static void *xmalloc(size_t size)
{
    void *result = malloc(size);

    if (result == NULL)
    {
        errx(1, "xmalloc: no more memory.");
    }

    return result;
}

static void *xrealloc(void *ptr, size_t size)
{
    void *result = realloc(ptr, size);

    if (result == NULL)
    {
        errx(1, "xrealloc: no more memory.");
    }

    return result;
}

static char *xstrdup(const char *string)
{
    size_t  size   = strlen(string) + 1;
    char   *result = malloc(size);

    if (result == NULL)
    {
        errx(1, "xstrdup: no more memory.");
    }

    memcpy(result, string, size);

    return result;
}

/*
 * Convert a string to an integer. The string should consist of a
 * number followed by an optional suffix of either k, m, g or t for
 * kilobytes, megabytes, gigabytes or terabytes respectively.
 * The unit sizes themselves are set via the define's at the start
 * of this file.
 */
static off_t string_to_number(char *string)
{
    char  *unit   = NULL;
    off_t  number = strtol(string, &unit, 10);

    if (unit == string)
    {
        errx(1, "invalid input.");
    }

    if (*unit == '\0')
    {
        return number;
    }

    /* unit should be one char, not more */
    if (unit[1] == '\0')
    {
        switch (tolower(*unit))
        {
            case 't': return number * TB;
            case 'g': return number * GB;
            case 'm': return number * MB;
            case 'k': return number * KB;
            case 'b': return number;
        }
    }

    errx(1, "unknown unit: '%s'", unit);
    return 0;
}

/*
 * Convert an integer to a 'human readable' string, e.g. converted to a
 * number with a suffix as explained by the comment for
 * string_to_number.
 */
static char *number_to_string(double number)
{
    char string[BUFSIZE] = { 0 };

    if      (number >= TB) sprintf(string, "%.2fT", number / TB);
    else if (number >= GB) sprintf(string, "%.2fG", number / GB);
    else if (number >= MB) sprintf(string, "%.2fM", number / MB);
    else if (number >= KB) sprintf(string, "%.2fK", number / KB);
    else                   sprintf(string, "%.0fB", number);

    return xstrdup(string);
}

/*
 * Strip consecutive and ending slashes from a path.
 * NOTE: realpath can not be used since the path may not exist.
 */
static char *cleanpath(char *path)
{
    char *buf    = xmalloc(strlen(path) + 1);
    char *bufpos = buf;
    char *result = NULL;

    while (*path != '\0')
    {
        if (*path == '/')
        {
            *bufpos++ = *path++;
            while (*path == '/')
            {
                ++path;
            }
        }
        else
        {
            *bufpos++ = *path++;
        }
    }

    if ((bufpos > buf + 1) && (bufpos[-1] == '/'))
    {
        bufpos[-1] = '\0';
    }
    else
    {
        *bufpos = '\0';
    }

    result = xstrdup(buf);
    free(buf);

    return result;
}

/*
 * A dynamic array which holds void pointers to 'size' data items. If
 * capacity 'limit' is reached adding to it will resize it to double
 * it's current capacity.
 */
struct array
{
    void   **items;
    size_t   size;
    size_t   limit;
};

#define INITIAL_ARRAY_LIMIT 64

static struct array *array_new(void)
{
    struct array *array = xmalloc(sizeof(*array));

    array->items = xmalloc(sizeof(void *) * INITIAL_ARRAY_LIMIT);
    array->limit = INITIAL_ARRAY_LIMIT;
    array->size  = 0;

    return array;
}

static void array_add(struct array *array, void *data)
{
    if (array->size == array->limit)
    {
        size_t new_limit = array->limit * 2;

        if (new_limit < array->limit)
        {
            errx(1, "array_add: overflow.");
        }

        array->items = xrealloc(array->items, sizeof(void *) * new_limit);
        array->limit = new_limit;
    }

    array->items[array->size++] = data;
}

/*
 * Release memory occupied by an array.  To release the items
 * themselves a function can be given which is called for each item in
 * the array.
 */
static void array_free(struct array *array, void (*free_function)(void *))
{
    if (free_function != NULL)
    {
        size_t item = 0;

        for (; item < array->size; ++item)
        {
            free_function(array->items[item]);
        }
    }

    free(array->items);
    free(array);
}

/*
 * To be able to fit files and present a disklist file_info stores the
 * size and the name of a file.
 */
struct file_info
{
    off_t  size;
    char  *name;
};

static struct file_info *file_info_new(char *name, off_t size)
{
    struct file_info *file_info = xmalloc(sizeof(struct file_info));

    file_info->name = name;
    file_info->size = size;

    return file_info;
}

static void file_info_free(void *file_info_ptr)
{
    struct file_info *file_info = file_info_ptr;

    free(file_info->name);
    free(file_info);
}

/*
 * A disk with 'free' free space contains a array of files. It's id
 * is an incrementing number so it doubles as the total number of
 * disks made.
 */
struct disk
{
    struct array *files;
    off_t         free;
    size_t        id;
};

static struct disk *disk_new(off_t size)
{
    static size_t  disk_id = 0;
    struct disk   *disk    = xmalloc(sizeof(*disk));

    disk->free  = size;
    disk->files = array_new();
    disk->id    = ++disk_id;

    return disk;
}

static void disk_free(void *disk_ptr)
{
    struct disk *disk = disk_ptr;

    /*
     * NOTE: Files are shared with the files array so we don't use a
     * free function to clean them up here; we
     * would double free otherwise.
     */
    array_free(disk->files, NULL);
    free(disk);
}

static void print_line(int length)
{
    int count = 0;

    for (; count < length; ++count)
    {
        putchar('-');
    }

    putchar('\n');
}

/*
 * Pretty print a disk and it's contents.
 */
static void disk_print(struct disk *disk)
{
    char    header[BUFSIZE] = { 0 };
    size_t  header_length   = 0;
    size_t  file_number     = 0;
    char   *size_string     = NULL;

    /* print a nice header */
    size_string = number_to_string(disk->free);
    sprintf(header,
            "Disk #%lu, %d%% (%s) free:",
            (unsigned long) disk->id,
            (int) (disk->free * 100 / g_disk_size), size_string);
    free(size_string);

    header_length = strlen(header);

    print_line(header_length);
    printf("%s\n", header);
    print_line(header_length);

    /* and the contents */
    for (file_number = 0; file_number < disk->files->size; ++file_number)
    {
        struct file_info *file_info = disk->files->items[file_number];

        size_string = number_to_string(file_info->size);
        printf("%10s %s\n", size_string, file_info->name);
        free(size_string);
    }
    putchar('\n');
}

/*
 * Create the directory given by path. If it already exists but it's
 * not a directory exit with an error message.
 */
static void make_dir(char *path)
{
    struct stat stat_buffer = { 0 };

    /* if path already exists it should be a directory */
    if (stat(path, &stat_buffer) == 0)
    {
        if (!S_ISDIR(stat_buffer.st_mode))
        {
            errx(1, "'%s' is not a directory.", path);
        }

        return;
    }

    if (mkdir(path, 0700) == -1)
    {
        err(1, "can't make directory '%s'", path);
    }
}

/*
 * Create any missing directories given by path.
 */
static void make_dirs(char *path)
{
    char *slashpos = path + 1;

    while ((slashpos = strchr(slashpos, '/')) != NULL)
    {
        *slashpos = '\0';
        make_dir(path);
        *slashpos++ = '/';
    }

    make_dir(path);
}

/*
 * Link the contents of a disk to the given destination directory.
 */
static void disk_link(struct disk *disk, char *destdir)
{
    char   *path        = NULL;
    char   *temp        = NULL;
    size_t  file_number = 0;

    if (disk->id > 9999)
    {
        errx(1, "disk_link: disk_id too big for format.");
    }

    path = xmalloc(strlen(destdir) + 6);
    sprintf(path, "%s/%04lu", destdir, (unsigned long) disk->id);
    temp = cleanpath(path);
    free(path);
    path = temp;

    for (file_number = 0; file_number < disk->files->size; ++file_number)
    {
        struct file_info *file_info = disk->files->items[file_number];
        char             *slashpos  = NULL;
        char             *destfile  = xmalloc(strlen(path)
                                            + strlen(file_info->name)
                                            + 2);

        sprintf(destfile, "%s/%s", path, file_info->name);
        slashpos = strrchr(destfile, '/');
        *slashpos = '\0';
        make_dirs(destfile);
        *slashpos = '/';

        if (link(file_info->name, destfile) == -1)
        {
            err(1, "can't link '%s' to '%s'", file_info->name, destfile);
        }

        printf("%s -> %s\n", file_info->name, path);
        free(destfile);
    }

    free(path);
}

static int compare_file_info(const void *a, const void *b)
{
    struct file_info *fa = *((struct file_info **) a);
    struct file_info *fb = *((struct file_info **) b);

    /* order by size, descending */
    return fb->size - fa->size;
}

/*
 * Fits files onto disks following a simple algorithm; first sort files
 * by size descending, then loop over the available disks for a fit. If
 * none can hold the file create a new disk containing it.  This will
 * rapidly fill disks while the smaller remaining files will usually
 * make a good final fit.
 */
static void fit_files(struct array *files, struct array *disks)
{
    size_t file_number = 0;

    qsort(files->items, files->size, sizeof(void *), compare_file_info);
    for (; file_number < files->size; ++file_number)
    {
        struct file_info *file_info   = files->items[file_number];
        int               added       = false;
        size_t            disk_number = 0;

        for (; disk_number < disks->size; ++disk_number)
        {
            struct disk *disk = disks->items[disk_number];

            if (disk->free - file_info->size >= 0)
            {
                array_add(disk->files, file_info);
                disk->free -= file_info->size;
                added = true;
                break;
            }
        }

        if (!added)
        {
            struct disk *disk = disk_new(g_disk_size);

            array_add(disk->files, file_info);
            disk->free -= file_info->size;
            array_add(disks, disk);
        }
    }
}

/*
 * Searches the given path for files to add to a container.
 * If recursive is true all underlying paths will also be searched.
 */
static void collect_files(char *path, struct array *files, int recursive)
{
    struct dirent *dir_entry = NULL;
    DIR           *dir_ptr   = opendir(path);

    if (dir_ptr == NULL)
    {
        err(1, "can't open directory '%s'", path);
    }

    for (dir_entry = readdir(dir_ptr);
         dir_entry != NULL;
         dir_entry = readdir(dir_ptr))
    {
        struct stat  statbuf  = { 0 };
        char        *fullname = NULL;

        if (strcmp(dir_entry->d_name, ".")  == 0
         || strcmp(dir_entry->d_name, "..") == 0)
        {
            continue;
        }

        fullname = xmalloc(strlen(path) + strlen(dir_entry->d_name) + 2);
        sprintf(fullname, "%s/%s", path, dir_entry->d_name);

        if (stat(fullname, &statbuf) != 0)
        {
            err(1, "can't access '%s'", fullname);
        }
        else if (S_ISDIR(statbuf.st_mode))
        {
            if (recursive)
            {
                collect_files(fullname, files, recursive);
            }
            free(fullname);
        }
        else if (S_ISREG(statbuf.st_mode))
        {
            struct file_info *file_info = NULL;

            if (statbuf.st_size > g_disk_size)
            {
                char *size_string = number_to_string(statbuf.st_size);

                errx(1, "can never fit '%s' (%s).", fullname, size_string);
            }

            file_info = file_info_new(fullname, statbuf.st_size);
            array_add(files, file_info);
        }
        else
        {
            err(1, "'%s': not a regular file.", fullname);
        }
    }

    closedir(dir_ptr);
}

static void usage(void)
{
    fprintf(stderr, "%s", g_fit_usage_string);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    char         *destdir     = NULL;
    struct array *files       = NULL;
    struct array *disks       = NULL;
    size_t        disk_number = 0;
    int           arg         = 0;
    int           opt         = 0;
    int           lflag       = false;
    int           nflag       = false;
    int           rflag       = false;
    int           sflag       = false;

    while ((opt = getopt(argc, argv, "hl:nrs:")) != -1)
    {
        switch (opt)
        {
            case 'h':
                usage();
                break;

            case 'l':
                destdir = cleanpath(optarg);
                lflag   = true;
                break;

            case 'n':
                nflag = true;
                break;

            case 'r':
                rflag = true;
                break;

            case 's':
                g_disk_size = string_to_number(optarg);
                sflag = true;
                break;

            case '?':
                usage();
        }
    }

    /* A path argument and the size option is mandatory. */
    if (optind >= argc || !sflag)
    {
        usage();
    }

    /* The given size should be positive */
    if (g_disk_size <= 0)
    {
        errx(1, "disk size is too small.");
    }

    files = array_new();
    for (arg = optind; arg < argc; ++arg)
    {
        char *path = cleanpath(argv[arg]);

        collect_files(path, files, rflag);
        free(path);
    }

    if (files->size == 0)
    {
        errx(1, "no files found.");
    }

    disks = array_new();
    fit_files(files, disks);

    /*
     * Be realistic about the number of disks to support, the helper
     * functions above assume a format string which will fit 4 digits.
     */
    if (disks->size > 9999)
    {
        errx(1, "fitting takes too many disks. (%lu)", disks->size);
    }

    if (nflag)
    {
        printf("%lu disk%s.\n",
               (unsigned long) disks->size,
               disks->size > 1 ? "s" : "");
        exit(EXIT_SUCCESS);
    }

    for (disk_number = 0; disk_number < disks->size; ++disk_number)
    {
        struct disk *disk = disks->items[disk_number];

        if (lflag)
        {
            disk_link(disk, destdir);
        }
        else
        {
            disk_print(disk);
        }
    }

    array_free(files, file_info_free);
    array_free(disks, disk_free);
    if (lflag)
    {
        free(destdir);
    }

    return EXIT_SUCCESS;
}

