/* RescueCopy - File and directory-level dd_rescue with recursive mirroring,
 * multi-source support, 16MB fast-paths, and 4KB/64KB surgical retries.
 * Copyright (C) 2020-2026 by Jody Bruchon <jody@jodybruchon.com>
 * Distributed under The MIT License
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#include <libjodycode.h>
#include "libjodycode_check.h"
#include "version.h"

/* Buffer and retry tier definitions */
#define CHUNK_LARGE  (16 * 1024 * 1024) /* 16MB default fast path for spinning rust */
#define CHUNK_MEDIUM (65536)            /* 64KB retry tier */
#define CHUNK_SMALL  (4096)             /* 4KB micro-retry floor */

/* Detect Windows and modify as needed */
#if defined _WIN32 || defined __MINGW32__
 #ifndef ON_WINDOWS
  #define ON_WINDOWS 1
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <io.h>
 #define mkdir(path, mode) _mkdir(path)
#else /* Not Windows */
 #ifdef UNICODE
  #error Do not define UNICODE on non-Windows platforms.
  #undef UNICODE
 #endif
#endif /* _WIN32 || __MINGW32__ */

static const char *program_name = "rescuecopy";
static char *large_buf = NULL;
static intmax_t total_read_errors = 0;
static intmax_t total_files_rescued = 0;

static void version(void)
{
	printf("rescuecopy %s (%s) by Jody Bruchon <jody@jodybruchon.com>\n", VER, VERDATE);
	printf("Linked to libjodycode %s (%s)\n", LIBJODYCODE_VER, LIBJODYCODE_VERDATE);
	printf("\nLatest versions and support:\n");
	printf("        https://github.com/jbruchon/rescuecopy\n");
	return;
}

static void usage(void)
{
	printf("Usage: %s source1 [source2 source3 ...] dest_path\n", program_name);
	printf("Copies multiple files or directories to a destination, zero-filling bad sectors on read errors.\n");
	return;
}

/* Recursive rescue block reader: tries large reads first, splits down to 4KB on read errors */
static int rescue_block(FILE *in, FILE *out, off_t offset, size_t size, char *buf, intmax_t *error_count, intmax_t *bytes_copied)
{
	if (size <= CHUNK_SMALL) {
		if (fseeko(in, offset, SEEK_SET) != 0) {
			memset(buf, 0, size);
			if (fwrite(buf, 1, size, out) != size) return -1;
			(*error_count)++;
			*bytes_copied += (intmax_t)size;
			return 0;
		}
		clearerr(in);
		size_t r = fread(buf, 1, size, in);
		if (ferror(in) || r < size) {
			memset(buf, 0, size);
			if (fwrite(buf, 1, size, out) != size) return -1;
			(*error_count)++;
			*bytes_copied += (intmax_t)size;
			return 0;
		}
		if (fwrite(buf, 1, size, out) != size) return -1;
		*bytes_copied += (intmax_t)size;
		return 1;
	}

	if (fseeko(in, offset, SEEK_SET) == 0) {
		clearerr(in);
		size_t r = fread(buf, 1, size, in);
		if (!ferror(in) && r == size) {
			if (fwrite(buf, 1, size, out) != size) return -1;
			*bytes_copied += (intmax_t)size;
			return (int)size;
		}
	}

	size_t half = size / 2;
	if (half < CHUNK_SMALL) half = CHUNK_SMALL;

	if (rescue_block(in, out, offset, half, buf, error_count, bytes_copied) < 0) return -1;
	if (size - half > 0) {
		if (rescue_block(in, out, offset + (off_t)half, size - half, buf, error_count, bytes_copied) < 0) return -1;
	}

	return (int)size;
}

/* Perform rescue copy on a single file */
static int rescue_single_file(const char *src, const char *dst)
{
	FILE *fp1, *fp2;
	struct JC_STAT stat1;
	intmax_t file_errors = 0;
	intmax_t processed = 0;

	fp1 = jc_fopen(src, JC_FILE_MODE_RDONLY_SEQ);
	if (!fp1) {
		fprintf(stderr, "\n[rescuecopy] Warning: cannot open input file: %s\n", src);
		return -1;
	}

	fp2 = jc_fopen(dst, JC_FILE_MODE_WRONLY_SEQ);
	if (!fp2) {
		fprintf(stderr, "\n[rescuecopy] Warning: cannot create output file: %s\n", dst);
		jc_fclose(fp1);
		return -1;
	}

	if (jc_stat(src, &stat1) != 0 || stat1.st_size == 0) {
		jc_fclose(fp1);
		jc_fclose(fp2);
		total_files_rescued++;
		return 0;
	}

	printf("[Rescue] Copying: %s -> %s\n", src, dst);

	off_t current_offset = 0;
	while (current_offset < stat1.st_size) {
		size_t chunk_to_read = (size_t)((stat1.st_size - current_offset) < CHUNK_LARGE ? (stat1.st_size - current_offset) : CHUNK_LARGE);

		int res = rescue_block(fp1, fp2, current_offset, chunk_to_read, large_buf, &file_errors, &processed);
		if (res < 0) {
			fprintf(stderr, "\n[rescuecopy] Error writing to destination: %s\n", dst);
			jc_fclose(fp1);
			jc_fclose(fp2);
			return -1;
		}

		current_offset += (off_t)chunk_to_read;
	}

	jc_fclose(fp1);
	jc_fclose(fp2);

	total_read_errors += file_errors;
	total_files_rescued++;
	if (file_errors > 0) {
		printf("  -> Finished with %" PRIdMAX " bad block(s) zero-filled.\n", file_errors);
	}
	return 0;
}

/* Recursively walk directories using libjodycode's JC_PATHBUF_SIZE and directory wrappers */
static int rescue_recursive(const char *src_dir, const char *dst_dir)
{
	JC_DIR *dir;
	struct JC_DIRENT *entry;
	struct JC_STAT st;

	dir = jc_opendir(src_dir);
	if (!dir) {
		fprintf(stderr, "Error opening directory: %s\n", src_dir);
		return -1;
	}

	/* Ensure destination directory exists */
	mkdir(dst_dir, 0777);

	while ((entry = jc_readdir(dir)) != NULL) {
		char src_path[JC_PATHBUF_SIZE];
		char dst_path[JC_PATHBUF_SIZE];

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		snprintf(src_path, JC_PATHBUF_SIZE, "%s/%s", src_dir, entry->d_name);
		snprintf(dst_path, JC_PATHBUF_SIZE, "%s/%s", dst_dir, entry->d_name);

		if (jc_stat(src_path, &st) != 0) {
			continue;
		}

		if (JC_S_ISDIR(st.st_mode)) {
			rescue_recursive(src_path, dst_path);
		} else if (JC_S_ISREG(st.st_mode)) {
			rescue_single_file(src_path, dst_path);
		}
	}

	jc_closedir(dir);
	return 0;
}

#ifdef UNICODE
int wmain(int argc, wchar_t **wargv)
#else
int main(int argc, char **argv)
#endif
{
	int stdout_tty = 0;
#ifdef ON_WINDOWS
 #ifdef UNICODE
  char **argv = NULL;
  int ut_err = jc_setup_unicode_terminal(argc, wargv, &argv, &stdout_tty);
 #else
  int ut_err = jc_setup_unicode_terminal(0, NULL, NULL, &stdout_tty);
 #endif /* UNICODE */
  if (ut_err != 0) {
    jc_print_error(ut_err);
    exit(EXIT_FAILURE);
  }
#else
  if (isatty(fileno(stdout))) stdout_tty = 1;
#endif /* ON_WINDOWS */

#ifdef ON_WINDOWS
	if (setvbuf(stdout, NULL, _IONBF, 0) != 0 || setvbuf(stderr, NULL, _IONBF, 0) != 0)
		fprintf(stderr, "warning: setvbuf() failed\n");
#endif

	if (libjodycode_version_check(1, 0) != 0) {
		exit(EXIT_FAILURE);
	}

	if (argv && argv[0]) {
		program_name = argv[0];
	}

	if (argc >= 2 && argv[1]) {
		if (jc_streq(argv[1], "-h") == 0 || jc_streq(argv[1], "--help") == 0) {
			usage();
			exit(EXIT_SUCCESS);
		}
		if (jc_streq(argv[1], "-v") == 0 || jc_streq(argv[1], "-V") == 0 || jc_streq(argv[1], "--version") == 0) {
			version();
			exit(EXIT_SUCCESS);
		}
	}

	if (argc < 3) {
		usage();
		exit(EXIT_FAILURE);
	}

	/* Allocate the 16MB buffer */
	large_buf = (char *)malloc(CHUNK_LARGE);
	if (!large_buf) {
		fprintf(stderr, "Error: out of memory allocating 16MB buffer.\n");
		exit(EXIT_FAILURE);
	}

	/* The destination is always the final argument */
	const char *dest_path = argv[argc - 1];
	struct JC_STAT st_dest;
	int dest_is_dir = 0;

	if (jc_stat(dest_path, &st_dest) == 0 && JC_S_ISDIR(st_dest.st_mode)) {
		dest_is_dir = 1;
	} else {
		if (argc > 3) {
			mkdir(dest_path, 0777);
			if (jc_stat(dest_path, &st_dest) == 0 && JC_S_ISDIR(st_dest.st_mode)) {
				dest_is_dir = 1;
			} else {
				fprintf(stderr, "Error: Multiple sources specified, but destination '%s' is not a directory.\n", dest_path);
				free(large_buf);
				exit(EXIT_FAILURE);
			}
		}
	}

	/* Loop through all sources (from index 1 up to argc - 2) */
	for (int x = 1; x < argc - 1; x++) {
		const char *src_path = argv[x];
		struct JC_STAT st_src;

		if (jc_stat(src_path, &st_src) != 0) {
			fprintf(stderr, "Error: source path does not exist: %s\n", src_path);
			continue;
		}

		if (JC_S_ISDIR(st_src.st_mode)) {
			char target_subdir[JC_PATHBUF_SIZE];
			const char *dirname_ptr = strrchr(src_path, '/');
			if (!dirname_ptr) dirname_ptr = strrchr(src_path, '\\');
			dirname_ptr = dirname_ptr ? dirname_ptr + 1 : src_path;

			if (dest_is_dir) {
				snprintf(target_subdir, JC_PATHBUF_SIZE, "%s/%s", dest_path, dirname_ptr);
			} else {
				snprintf(target_subdir, JC_PATHBUF_SIZE, "%s", dest_path);
			}
			printf("[rescuecopy] Starting recursive directory mirror: %s -> %s\n", src_path, target_subdir);
			rescue_recursive(src_path, target_subdir);
		} else {
			char final_dst[JC_PATHBUF_SIZE];
			if (dest_is_dir) {
				const char *filename = strrchr(src_path, '/');
				if (!filename) filename = strrchr(src_path, '\\');
				filename = filename ? filename + 1 : src_path;
				snprintf(final_dst, JC_PATHBUF_SIZE, "%s/%s", dest_path, filename);
			} else {
				snprintf(final_dst, JC_PATHBUF_SIZE, "%s", dest_path);
			}
			rescue_single_file(src_path, final_dst);
		}
	}

	free(large_buf);

	printf("\n[rescuecopy] Complete! Rescued %" PRIdMAX " file(s) with a total of %" PRIdMAX " bad block error(s) zero-filled.\n",
			total_files_rescued, total_read_errors);

	exit(EXIT_SUCCESS);
}