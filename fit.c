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
#define _BSD_SOURCE
#define _GNU_SOURCE

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

#define BUFSIZE 4096

static off_t g_disk_size = 0;

enum { false, true };

/*
 * The next couple of functions are wrappers around their
 * c stdlib/posix counterparts which exit on error.
 */
static void *
xmalloc(size_t size)
{
	void *ret;

	ret = malloc(size);
	if (ret == NULL)
		errx(1, "xmalloc: no more memory.");

	return ret;
}

static void *
xrealloc(void *ptr, size_t size)
{
	void *ret;

	ret = realloc(ptr, size);
	if (ret == NULL)
		errx(1, "xrealloc: no more memory.");

	return ret;
}

static char *
xstrdup(const char *str)
{
	size_t size;
	char *ret;

	size = strlen(str) + 1;
	ret = malloc(size);
	if (ret == NULL)
		errx(1, "xstrdup: no more memory.");

	memcpy(ret, str, size);

	return ret;
}

/*
 * Convert a string to an integer. The string should consist of a
 * number followed by an optional suffix of either k, m, g or t for
 * kilobytes, megabytes, gigabytes or terabytes respectively.
 * The unit sizes themselves are set via the define's at the start
 * of this file.
 */
static off_t
string_to_number(char *str)
{
	char *unit;
	off_t num;

	num = strtol(str, &unit, 10);

	if (unit == str)
		errx(1, "invalid input.");

	if (*unit == '\0')
		return num;

	/* unit should be one char, not more */
	if (unit[1] == '\0') {
		switch (tolower(*unit)) {
		case 't':
			return num * TB;

		case 'g':
			return num * GB;

		case 'm':
			return num * MB;

		case 'k':
			return num * KB;

		case 'b':
			return num;
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
static char *
number_to_string(double num)
{
	char buf[BUFSIZE];

	if (num >= TB)
		sprintf(buf, "%.2fT", num / TB);
	else if (num >= GB)
		sprintf(buf, "%.2fG", num / GB);
	else if (num >= MB)
		sprintf(buf, "%.2fM", num / MB);
	else if (num >= KB)
		sprintf(buf, "%.2fK", num / KB);
	else
		sprintf(buf, "%.0fB", num);

	return xstrdup(buf);
}

/*
 * Strip consecutive and ending slashes from a path.
 * NOTE: realpath can not be used since the path may not exist.
 */
static char *
normalize_path(char *path)
{
	char *buf, *pos, *ret;

	buf = pos = xmalloc(strlen(path) + 1);

	while (*path != '\0') {
		if (*path == '/') {
			*pos++ = *path++;
			while (*path == '/')
				++path;
		} else
			*pos++ = *path++;
	}

	if ((pos > buf + 1) && (pos[-1] == '/'))
		pos[-1] = '\0';
	else
		*pos = '\0';

	ret = xstrdup(buf);
	free(buf);

	return ret;
}

/*
 * A dynamic array which holds void pointers to 'size' data items. If
 * capacity 'cap' is reached adding to it will resize it to double
 * it's current capacity.
 */
struct array {
	void **items;
	size_t size;
	size_t cap;
};

static struct array *
array_new(void)
{
	struct array *a;

#define INITIAL_ARRAY_CAPACITY 64
	a = xmalloc(sizeof(struct array));
	a->items = xmalloc(sizeof(void *) * INITIAL_ARRAY_CAPACITY);
	a->cap = INITIAL_ARRAY_CAPACITY;
	a->size = 0;

	return a;
}

static void
array_add(struct array *a, void *data)
{
	if (a->size == a->cap) {
		size_t newcap = a->cap * 2;

		a->items = xrealloc(a->items, sizeof(void *) * newcap);
		a->cap = newcap;
	}

	a->items[a->size++] = data;
}

/*
 * Release memory occupied by an array.  To release the items
 * themselves a function can be given which is called for each item in
 * the array.
 */
static void
array_free(struct array *a, void (*f)(void *))
{
	if (f != NULL) {
		size_t i;

		for (i = 0; i < a->size; ++i)
			f(a->items[i]);
	}

	free(a->items);
	free(a);
}

/*
 * To be able to fit files and present a disklist fileinfo stores the
 * size and the name of a file.
 */
struct fileinfo {
	off_t size;
	char *name;
};

static struct fileinfo *
fileinfo_new(char *name, off_t size)
{
	struct fileinfo *file;

	file = xmalloc(sizeof(struct fileinfo));
	file->name = name;
	file->size = size;

	return file;
}

static void
fileinfo_free(void *fileinfo_ptr)
{
	struct fileinfo *file = fileinfo_ptr;

	free(file->name);
	free(file);
}

/*
 * A disk with 'free' free space contains a array of files. It's id
 * is an incrementing number so it doubles as the total number of
 * disks made.
 */
struct disk {
	struct array *files;
	off_t free;
	size_t id;
};

static struct disk *
disk_new(off_t size)
{
	static size_t id = 0;
	struct disk *disk;

	disk = xmalloc(sizeof(struct disk));
	disk->free = size;
	disk->files = array_new();
	disk->id = ++id;

	return disk;
}

static void
disk_free(void *disk_ptr)
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

static void
print_line(int len)
{
	int i;

	for (i = 0; i < len; ++i)
		putchar('-');

	putchar('\n');
}

/*
 * Pretty print a disk and it's contents.
 */
static void
disk_print(struct disk *disk)
{
	char hdr[BUFSIZE];
	size_t hdrlen, i;
	char *sizestr;

	/* print a nice header */
	sizestr = number_to_string(disk->free);
	sprintf(hdr, "Disk #%lu, %d%% (%s) free:",
	    (unsigned long) disk->id,
	    (int) (disk->free * 100 / g_disk_size), sizestr);
	free(sizestr);

	hdrlen = strlen(hdr);

	print_line(hdrlen);
	printf("%s\n", hdr);
	print_line(hdrlen);

	/* and the contents */
	for (i = 0; i < disk->files->size; ++i) {
		struct fileinfo *file = disk->files->items[i];

		sizestr = number_to_string(file->size);
		printf("%10s %s\n", sizestr, file->name);
		free(sizestr);
	}
	putchar('\n');
}

/*
 * Create the directory given by path. If it already exists but it's
 * not a directory exit with an error message.
 */
static void
make_dir(char *path)
{
	struct stat sb;

	/* if path already exists it should be a directory */
	if (stat(path, &sb) == 0) {
		if (!S_ISDIR(sb.st_mode))
			errx(1, "'%s' is not a directory.", path);

		return;
	}

	if (mkdir(path, 0700) == -1)
		err(1, "can't make directory '%s'", path);
}

/*
 * Create any missing directories given by path.
 */
static void
make_dirs(char *path)
{
	char *slashpos = path + 1;

	while ((slashpos = strchr(slashpos, '/')) != NULL) {
		*slashpos = '\0';
		make_dir(path);
		*slashpos++ = '/';
	}

	make_dir(path);
}

/*
 * Link the contents of a disk to the given destination directory.
 */
static void
disk_link(struct disk *disk, char *destdir)
{
	char *path;
	char *temp;
	size_t i;

	if (disk->id > 9999)
		errx(1, "disk_link: disk_id too big for format.");

	path = xmalloc(strlen(destdir) + 6);
	sprintf(path, "%s/%04lu", destdir, (unsigned long) disk->id);
	temp = normalize_path(path);
	free(path);
	path = temp;

	for (i = 0; i < disk->files->size; ++i) {
		struct fileinfo *file = disk->files->items[i];
		char *destfile;
		char *slashpos;

		destfile = xmalloc(strlen(path) + strlen(file->name) + 2);
		sprintf(destfile, "%s/%s", path, file->name);

		slashpos = strrchr(destfile, '/');
		*slashpos = '\0';
		make_dirs(destfile);
		*slashpos = '/';

		if (link(file->name, destfile) == -1)
			err(1, "can't link '%s' to '%s'", file->name,
			    destfile);

		printf("%s -> %s\n", file->name, path);
		free(destfile);
	}

	free(path);
}

static int
compare_fileinfo(const void *a, const void *b)
{
	struct fileinfo *fa = *((struct fileinfo **) a);
	struct fileinfo *fb = *((struct fileinfo **) b);

	/* order by size, descending */
	if (fa->size < fb->size)
		return 1;
	else if (fa->size > fb->size)
		return -1;

	return 0;
}

/*
 * Fits files onto disks following a simple algorithm; first sort files
 * by size descending, then loop over the available disks for a fit. If
 * none can hold the file create a new disk containing it.  This will
 * rapidly fill disks while the smaller remaining files will usually
 * make a good final fit.
 */
static void
fit_files(struct array *files, struct array *disks)
{
	size_t i;

	qsort(files->items, files->size, sizeof(void *), compare_fileinfo);

	for (i = 0; i < files->size; ++i) {
		struct fileinfo *file = files->items[i];
		int added = false;
		size_t j;

		for (j = 0; j < disks->size; ++j) {
			struct disk *disk = disks->items[j];

			if (disk->free - file->size >= 0) {
				array_add(disk->files, file);
				disk->free -= file->size;
				added = true;
				break;
			}
		}

		if (!added) {
			struct disk *disk;

			disk = disk_new(g_disk_size);
			array_add(disk->files, file);
			disk->free -= file->size;
			array_add(disks, disk);
		}
	}
}

/*
 * Searches the given path for files to add to a container.
 * If recursive is true all underlying paths will also be searched.
 */
static void
collect_files(char *path, struct array *files, int recursive)
{
	struct dirent *de;
	DIR *dp;

	dp = opendir(path);
	if (dp == NULL)
		err(1, "can't open directory '%s'", path);

	while ((de = readdir(dp)) != NULL) {
		struct stat sb;
		char *fullname;

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;

		fullname = xmalloc(strlen(path) + strlen(de->d_name) + 2);
		sprintf(fullname, "%s/%s", path, de->d_name);

		if (stat(fullname, &sb) != 0)
			err(1, "can't access '%s'", fullname);

		if (S_ISDIR(sb.st_mode)) {
			if (recursive)
				collect_files(fullname, files, recursive);
			free(fullname);
		} else {
			struct fileinfo *file;

			if (sb.st_size > g_disk_size) {
				char *sizestr;

				sizestr = number_to_string(sb.st_size);
				errx(1, "can never fit '%s' (%s).",
				    fullname, sizestr);
			}

			file = fileinfo_new(fullname, sb.st_size);
			array_add(files, file);
		}
	}

	closedir(dp);
}

static void
usage(void)
{
	fprintf(stderr, "%s", g_fit_usage_string);
	exit(EXIT_FAILURE);
}

/*
 * Program options are stored as a bitset.
 */
#define OPT_LINK	1
#define OPT_SHOW_ONLY	2
#define OPT_RECURSIVE	4
#define OPT_SIZE	8

int
main(int argc, char **argv)
{
	char *destdir;
	struct array *files, *disks;
	size_t i;
	int arg, opt, options;

	destdir = NULL;
	options = 0;
	while ((opt = getopt(argc, argv, "hl:nrs:")) != -1) {
		switch (opt) {
		case 'h':
			usage();
			break;

		case 'l':
			destdir = normalize_path(optarg);
			options |= OPT_LINK;
			break;

		case 'n':
			options |= OPT_SHOW_ONLY;
			break;

		case 'r':
			options |= OPT_RECURSIVE;
			break;

		case 's':
			g_disk_size = string_to_number(optarg);
			options |= OPT_SIZE;
			break;

		case '?':
			exit(EXIT_FAILURE);
		}
	}

	/* A path argument and the size option is mandatory. */
	if (optind >= argc || !(options & OPT_SIZE))
		usage();

	/* The given size should be positive */
	if (g_disk_size <= 0)
		errx(1, "disk size is too small.");

	files = array_new();
	for (arg = optind; arg < argc; ++arg) {
		char *path;

		path = normalize_path(argv[arg]);
		collect_files(path, files, options & OPT_RECURSIVE);
		free(path);
	}

	if (files->size == 0)
		errx(1, "no files found.");

	disks = array_new();
	fit_files(files, disks);

	/*
	 * Be realistic about the number of disks to support, the helper
	 * functions above assume a format string which will fit 4 digits.
	 */
	if (disks->size > 9999)
		errx(1, "fitting takes too many disks. (> 9999)");

	if (options & OPT_SHOW_ONLY) {
		printf("%lu disk%s.\n", (unsigned long) disks->size,
		    disks->size > 1 ? "s" : "");
		exit(EXIT_SUCCESS);
	}

	for (i = 0; i < disks->size; ++i) {
		struct disk *disk = disks->items[i];

		if (options & OPT_LINK)
			disk_link(disk, destdir);
		else
			disk_print(disk);
	}

	array_free(files, fileinfo_free);
	array_free(disks, disk_free);
	if (options & OPT_LINK)
		free(destdir);

	return EXIT_SUCCESS;
}
