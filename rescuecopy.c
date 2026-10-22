/* rescuecopy: a file copy tool that skips bad blocks instead of failing
 * Copyright (C) 2026 by Jody Bruchon <jody@jodybruchon.com>
 * Distributed under the GNU General Public License version 2
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


/* Buffer and retry tier definitions */
#define CHUNK_LARGE  16777216 /* 16MB fast copy buffer*/
#define CHUNK_MEDIUM 65536    /* 64KB mid-tier retry */
#define CHUNK_SMALL  4096     /* 4KB sector-sized lowest-tier retry */

static const char *program_name = "rescuecopy";
static char *large_buf = NULL;
static intmax_t total_read_errors = 0;
static intmax_t total_files_rescued = 0;
static intmax_t total_folders_processed = 0;
static int stderr_tty = 0;


static void version(int verbose)
{
	printf("rescuecopy %s (%s) by Jody Bruchon <jody@jodybruchon.com>\n", VER, VERDATE);
	if (verbose == 0) return;
	printf("Linked to libjodycode %s (%s)\n", LIBJODYCODE_VER, LIBJODYCODE_VERDATE);
	printf("Latest versions and support: https://codeberg.org/jbruchon/rescuecopy\n");
	printf("Please report any rescuecopy bugs to jody@jodybruchon.com\n");
	return;
}


static void usage(int exit_status)
{
	version(1);
	printf("\nUsage: %s source1 [source2 source3 ...] dest_path\n", program_name);
	printf("Copies multiple files or directories to a destination, zero-filling bad sectors on read errors.\n");
	exit(exit_status);
}


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
	if ((size - half > 0) && (rescue_block(in, out, offset + (off_t)half, size - half, buf, error_count, bytes_copied) < 0)) return -1;

	return (int)size;
}


static void update_progress(void)
{
	if (!stderr_tty) return;
	fprintf(stderr, "\r[rescuecopy] Folders: %" PRIdMAX " | Files Rescued: %" PRIdMAX " | Bad Blocks: %" PRIdMAX,
			total_folders_processed, total_files_rescued, total_read_errors);
	fflush(stderr);
}


static int rescue_single_file(const char *src, const char *dst)
{
	FILE *fp1, *fp2;
	struct JC_STAT stat1;
	intmax_t file_errors = 0;
	intmax_t processed = 0;

	fp1 = jc_fopen(src, JC_FILE_MODE_RDONLY_SEQ);
	if (!fp1) {
		if (stderr_tty) fprintf(stderr, "\r");
		fprintf(stderr, "\n[rescuecopy] Warning: cannot open input file: ");
		jc_fwprint(stderr, src, 1);
		update_progress();
		return -1;
	}

	fp2 = jc_fopen(dst, JC_FILE_MODE_WRONLY_SEQ);
	if (!fp2) {
		if (stderr_tty) fprintf(stderr, "\r");
		fprintf(stderr, "\n[rescuecopy] Warning: cannot create output file: ");
		jc_fwprint(stderr, dst, 1);
		jc_fclose(fp1);
		update_progress();
		return -1;
	}

	if (jc_stat(src, &stat1) != 0 || stat1.st_size == 0) {
		jc_fclose(fp1);
		jc_fclose(fp2);
		total_files_rescued++;
		update_progress();
		return 0;
	}

	off_t current_offset = 0;
	while (current_offset < stat1.st_size) {
		size_t chunk_to_read = (size_t)((stat1.st_size - current_offset) < CHUNK_LARGE ? (stat1.st_size - current_offset) : CHUNK_LARGE);

		int res = rescue_block(fp1, fp2, current_offset, chunk_to_read, large_buf, &file_errors, &processed);
		if (res < 0) {
			if (stderr_tty) fprintf(stderr, "\r");
			fprintf(stderr, "\n[rescuecopy] Error writing to destination: ");
			jc_fwprint(stderr, dst, 1);
			jc_fclose(fp1);
			jc_fclose(fp2);
			update_progress();
			return -1;
		}

		current_offset += (off_t)chunk_to_read;
	}

	jc_fclose(fp1);
	jc_fclose(fp2);

	total_read_errors += file_errors;
	total_files_rescued++;
	update_progress();
	return 0;
}


static int rescue_recursive(const char *src_dir, const char *dst_dir)
{
	JC_DIR *dir;
	struct JC_DIRENT *entry;
	struct JC_STAT st;

	dir = jc_opendir(src_dir);
	if (!dir) {
		if (stderr_tty) fprintf(stderr, "\r");
		fprintf(stderr, "Error opening directory: ");
		jc_fwprint(stderr, src_dir, 1);
		update_progress();
		return -1;
	}

	total_folders_processed++;
	update_progress();

	mkdir(dst_dir, 0700);

	while ((entry = jc_readdir(dir)) != NULL) {
		char src_path[JC_PATHBUF_SIZE];
		char dst_path[JC_PATHBUF_SIZE];

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

		snprintf(src_path, JC_PATHBUF_SIZE, "%s/%s", src_dir, entry->d_name);
		snprintf(dst_path, JC_PATHBUF_SIZE, "%s/%s", dst_dir, entry->d_name);

		if (jc_stat(src_path, &st) != 0) continue;

		if (JC_S_ISDIR(st.st_mode)) rescue_recursive(src_path, dst_path);
		else if (JC_S_ISREG(st.st_mode)) rescue_single_file(src_path, dst_path);
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
	if (!_isatty(_fileno(stderr))) stderr_tty = 0;
	else stderr_tty = 1;
#else
	if (isatty(fileno(stdout))) stdout_tty = 1;
	if (isatty(fileno(stderr))) stderr_tty = 1;
#endif /* ON_WINDOWS */

#ifdef ON_WINDOWS
	if (setvbuf(stdout, NULL, _IONBF, 0) != 0 || setvbuf(stderr, NULL, _IONBF, 0) != 0)
		fprintf(stderr, "warning: setvbuf() failed\n");
#endif

	if (libjodycode_version_check(1, 0) != 0) exit(EXIT_FAILURE);

	if (argv && argv[0]) {
		program_name = argv[0];
	}

	if (argc >= 2 && argv[1]) {
		if (jc_streq(argv[1], "-h") == 0 || jc_streq(argv[1], "--help") == 0) usage(EXIT_SUCCESS);
		if (jc_streq(argv[1], "-v") == 0 || jc_streq(argv[1], "-V") == 0 || jc_streq(argv[1], "--version") == 0) {
			version(1);
			exit(EXIT_SUCCESS);
		}
	}

	if (argc < 3) usage(EXIT_FAILURE);

	large_buf = (char *)malloc(CHUNK_LARGE);
	if (!large_buf) jc_oom("16MB main buffer allocation");

	const char *dest_path = argv[argc - 1];
	struct JC_STAT st_dest;
	int dest_is_dir = 0;

	if (jc_stat(dest_path, &st_dest) == 0 && JC_S_ISDIR(st_dest.st_mode)) {
		dest_is_dir = 1;
	} else {
		if (argc > 3) {
			mkdir(dest_path, 0700);
			if (jc_stat(dest_path, &st_dest) == 0 && JC_S_ISDIR(st_dest.st_mode)) {
				dest_is_dir = 1;
			} else {
				fprintf(stderr, "Error: Multiple sources specified, but destination '");
				jc_fwprint(stderr, dest_path, 0);
				fprintf(stderr, "' is not a directory.\n");
				free(large_buf);
				exit(EXIT_FAILURE);
			}
		}
	}

	update_progress();

	for (int x = 1; x < argc - 1; x++) {
		const char *src_path = argv[x];
		struct JC_STAT st_src;

		if (jc_stat(src_path, &st_src) != 0) {
			if (stderr_tty) fprintf(stderr, "\r");
			fprintf(stderr, "Error: source path does not exist: ");
			jc_fwprint(stderr, src_path, 1);
			update_progress();
			continue;
		}

		if (JC_S_ISDIR(st_src.st_mode)) {
			char target_subdir[JC_PATHBUF_SIZE];
			const char *dirname_ptr = strrchr(src_path, '/');

			if (!dirname_ptr) dirname_ptr = strrchr(src_path, '\\');
			dirname_ptr = dirname_ptr ? dirname_ptr + 1 : src_path;

			if (dest_is_dir) snprintf(target_subdir, JC_PATHBUF_SIZE, "%s/%s", dest_path, dirname_ptr);
			else snprintf(target_subdir, JC_PATHBUF_SIZE, "%s", dest_path);

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

	if (stderr_tty) fprintf(stderr, "\r%80s\r", "");

	fprintf(stderr, "[rescuecopy] Complete!\n");
	fprintf(stderr, "%" PRIdMAX " directories copied\n%" PRIdMAX " files copied\n%" PRIdMAX " bad blocks skipped\n",
			total_folders_processed, total_files_rescued, total_read_errors);

	exit(EXIT_SUCCESS);
}
