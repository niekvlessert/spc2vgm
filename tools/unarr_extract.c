#include "unarr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define PATH_SEPARATOR '\\'
#else
#include <sys/stat.h>
#define PATH_SEPARATOR '/'
#endif

static ar_archive *open_archive(ar_stream *stream)
{
	ar_archive *archive = ar_open_rar_archive(stream);
	if (!archive) archive = ar_open_zip_archive(stream, false);
	if (!archive) archive = ar_open_7z_archive(stream);
	if (!archive) archive = ar_open_tar_archive(stream);
	return archive;
}

static const char *base_name(const char *name)
{
	const char *base = name;
	for (const char *p = name; *p; ++p)
		if (*p == '/' || *p == '\\') base = p + 1;
	return base;
}

static int has_spc_extension(const char *name)
{
	const size_t length = strlen(name);
	if (length < 4 || name[length - 4] != '.') return 0;
	const char a = name[length - 3], b = name[length - 2], c = name[length - 1];
	return (a == 's' || a == 'S') && (b == 'p' || b == 'P') && (c == 'c' || c == 'C');
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "Usage: %s ARCHIVE OUTPUT_DIRECTORY\n", argv[0]);
		return 2;
	}
	ar_stream *stream = ar_open_file(argv[1]);
	if (!stream) {
		fprintf(stderr, "Unable to open archive: %s\n", argv[1]);
		return 1;
	}
	ar_archive *archive = open_archive(stream);
	if (!archive) {
		fprintf(stderr, "Unsupported or invalid archive: %s\n", argv[1]);
		ar_close(stream);
		return 1;
	}

	int extracted = 0, failed = 0;
	while (ar_parse_entry(archive)) {
		const char *name = ar_entry_get_name(archive);
		const char *base = name ? base_name(name) : "";
		if (!*base || !has_spc_extension(base)) continue;
		char stored_name[1024];
		if (snprintf(stored_name, sizeof(stored_name), "%s", base) >= (int)sizeof(stored_name)) {
			fprintf(stderr, "Filename too long: %s\n", base);
			failed = 1;
			break;
		}
		char path[4096];
		if (snprintf(path, sizeof(path), "%s%c%s", argv[2], PATH_SEPARATOR, stored_name) >= (int)sizeof(path)) {
			fprintf(stderr, "Path too long: %s\n", stored_name);
			failed = 1;
			break;
		}
		FILE *output = fopen(path, "wb");
		if (!output) {
			fprintf(stderr, "Unable to create: %s\n", path);
			failed = 1;
			break;
		}
		size_t remaining = ar_entry_get_size(archive);
		unsigned char buffer[65536];
		while (remaining) {
			const size_t count = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
			if (!ar_entry_uncompress(archive, buffer, count) || fwrite(buffer, 1, count, output) != count) {
				fprintf(stderr, "Unable to extract: %s\n", stored_name);
				failed = 1;
				break;
			}
			remaining -= count;
		}
		fclose(output);
		if (failed) break;
		printf("Extracted %s\n", stored_name);
		++extracted;
	}
	if (!failed && !ar_at_eof(archive)) {
		fprintf(stderr, "Archive parsing failed\n");
		failed = 1;
	}
	ar_close_archive(archive);
	ar_close(stream);
	if (!failed && !extracted) {
		fprintf(stderr, "Archive contains no SPC files\n");
		return 1;
	}
	if (!failed) printf("Extracted %d SPC files\n", extracted);
	return failed;
}
