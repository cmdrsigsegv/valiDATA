#include "watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE_BYTES (16L * 1024 * 1024)

static bool is_space(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_space(const char *text) {
	while (*text && is_space(*text)) {
		text++;
	}
	return text;
}

/* Copies [begin, end) without the whitespace around it. */
static void copy_trimmed(char *out, size_t size, const char *begin, const char *end) {
	size_t length;
	while (begin < end && is_space(*begin)) {
		begin++;
	}
	while (end > begin && is_space(end[-1])) {
		end--;
	}
	length = (size_t)(end - begin);
	if (length >= size) {
		length = size - 1;
	}
	memcpy(out, begin, length);
	out[length] = '\0';
}

static bool has_space(const char *begin, const char *end) {
	while (begin < end && is_space(*begin)) {
		begin++;
	}
	while (end > begin && is_space(end[-1])) {
		end--;
	}
	for (; begin < end; begin++) {
		if (is_space(*begin)) {
			return true;
		}
	}
	return false;
}

/* The options of a scheduler directive (#SBATCH, #PBS, #$, #BSUB, #FLUX: ...),
   or NULL when the line is not one. */
static const char *directive_options(const char *line) {
	const char *p = line + 1;
	if (line[0] != '#') {
		return NULL;
	}
	if (*p == '$') {
		p++;
	} else {
		if (*p < 'A' || *p > 'Z') {
			return NULL;
		}
		while (*p >= 'A' && *p <= 'Z') {
			p++;
		}
		if (*p == ':') {
			p++;
		}
	}
	if (*p != ' ' && *p != '\t') {
		return NULL;
	}
	p = skip_space(p);
	return *p == '-' ? p : NULL;
}

/* Steps over a leading word such as "export". */
static const char *skip_word(const char *text, const char *word) {
	size_t length = strlen(word);
	if (strncmp(text, word, length) == 0 && (text[length] == ' ' || text[length] == '\t')) {
		return skip_space(text + length);
	}
	return text;
}

typedef struct {
	const char *name;
	const char *name_end;
	const char *value;
	const char *value_end;
} Spans;

static void trim(const char **begin, const char **end) {
	while (*begin < *end && is_space(**begin)) {
		(*begin)++;
	}
	while (*end > *begin && is_space((*end)[-1])) {
		(*end)--;
	}
}

/* Where a comment starts in [begin, end), or end: a #, ! or // that follows
   whitespace and is not inside quotes. */
static const char *comment_start(const char *begin, const char *end) {
	char        quote = '\0';
	const char *p;
	for (p = begin; p < end; p++) {
		if (quote) {
			if (*p == quote) {
				quote = '\0';
			}
		} else if (*p == '"' || *p == '\'') {
			quote = *p;
		} else if (p > begin && is_space(p[-1]) &&
				   (*p == '#' || (*p == '!' && p[1] != '=') || (*p == '/' && p[1] == '/'))) {
			return p;
		}
	}
	return end;
}

static bool set_spans(Spans *spans, const char *name, const char *name_end, const char *value,
	const char *value_end) {
	trim(&name, &name_end);
	trim(&value, &value_end);
	value_end = comment_start(value, value_end);
	trim(&value, &value_end);
	if (value_end > value && (value_end[-1] == ',' || value_end[-1] == ';')) {
		value_end--; /* dt = 0.01, */
		trim(&value, &value_end);
	}
	spans->name      = name;
	spans->name_end  = name_end;
	spans->value     = value;
	spans->value_end = value_end;
	return true;
}

/* Where the name and the value of a line are, without the whitespace around
   them. False for a blank line. */
static bool find_spans(const char *line, Spans *spans) {
	const char *end;
	const char *options;
	const char *split;

	line = skip_space(line);
	end  = line + strlen(line);
	if (*line == '\0') {
		return false;
	}

	options = directive_options(line);
	if (options) {
		/* --nodes=4 or -N 4: the option is the name. */
		split = options;
		while (*split && !is_space(*split) && *split != '=') {
			split++;
		}
		return set_spans(spans, options, split, *split == '=' ? split + 1 : split, end);
	}

	line = skip_word(line, "export");
	line = skip_word(line, "setenv");
	line = skip_word(line, "set");

	/* name = value, when the name is one word. */
	split = strchr(line, '=');
	if (split && split > line && !has_space(line, split)) {
		return set_spans(spans, line, split, split + 1, end);
	}

	/* name: value */
	for (split = strchr(line, ':'); split; split = strchr(split + 1, ':')) {
		if (split[1] == '\0' || is_space(split[1])) {
			if (split > line && !has_space(line, split)) {
				return set_spans(spans, line, split, split + 1, end);
			}
			break;
		}
	}

	/* name value */
	split = line;
	while (*split && !is_space(*split)) {
		split++;
	}
	return set_spans(spans, line, split, split, end);
}

bool watch_parse_line(
	const char *line, char *name, size_t name_size, char *value, size_t value_size) {
	Spans spans;
	name[0]  = '\0';
	value[0] = '\0';
	if (!find_spans(line, &spans)) {
		return false;
	}
	copy_trimmed(name, name_size, spans.name, spans.name_end);
	copy_trimmed(value, value_size, spans.value, spans.value_end);
	return true;
}

bool watch_line_set_value(const char *line, const char *value, char *out, size_t size) {
	Spans  spans;
	size_t before;
	size_t length = strlen(value);
	size_t after;

	if (!find_spans(line, &spans) || spans.value == spans.value_end) {
		return false;
	}
	before = (size_t)(spans.value - line);
	after  = strlen(spans.value_end);
	if (before + length + after + 1 > size) {
		return false;
	}
	memcpy(out, line, before);
	memcpy(out + before, value, length);
	memcpy(out + before + length, spans.value_end, after + 1);
	return true;
}

/* A whole file as it is on disk, with a 0 after it. NULL when it cannot be read. */
static char *read_whole(const char *path, long *size) {
	FILE *stream = fopen(path, "rb");
	char *data;

	if (!stream) {
		return NULL;
	}
	if (fseek(stream, 0, SEEK_END) != 0 || (*size = ftell(stream)) < 0 || *size > MAX_FILE_BYTES ||
		fseek(stream, 0, SEEK_SET) != 0) {
		fclose(stream);
		return NULL;
	}
	data = malloc((size_t)*size + 1);
	if (!data || fread(data, 1, (size_t)*size, stream) != (size_t)*size) {
		fclose(stream);
		free(data);
		return NULL;
	}
	fclose(stream);
	data[*size] = '\0';
	return data;
}

bool watch_file_set_value(const char *path, int line, const char *value) {
	long   size;
	char  *data = read_whole(path, &size);
	char  *begin;
	char  *end;
	char  *old_line = NULL;
	char  *new_line = NULL;
	size_t length;
	size_t room;
	FILE  *stream;
	bool   done = false;

	if (!data) {
		return false;
	}
	begin = data;
	while (line > 1 && (begin = strchr(begin, '\n')) != NULL) {
		begin++;
		line--;
	}
	if (line != 1 || !begin || begin >= data + size) {
		free(data);
		return false;
	}
	end = strchr(begin, '\n'); /* a \r before it stays in the line, after the value */
	if (!end) {
		end = data + size;
	}
	length   = (size_t)(end - begin);
	room     = length + strlen(value) + 1;
	old_line = malloc(length + 1);
	new_line = malloc(room);
	if (old_line && new_line) {
		memcpy(old_line, begin, length);
		old_line[length] = '\0';
		if (watch_line_set_value(old_line, value, new_line, room) &&
			(stream = fopen(path, "wb")) != NULL) {
			size_t before = (size_t)(begin - data);
			size_t after  = (size_t)(data + size - end);
			done          = fwrite(data, 1, before, stream) == before;
			done          = fputs(new_line, stream) >= 0 && done;
			done          = fwrite(end, 1, after, stream) == after && done;
			done          = fclose(stream) == 0 && done;
		}
	}
	free(old_line);
	free(new_line);
	free(data);
	return done;
}

bool watch_file_load(WatchFile *file, const char *path) {
	long  size;
	long  i;
	int   count = 0;
	char *start;

	memset(file, 0, sizeof(*file));
	file->data = read_whole(path, &size);
	if (!file->data) {
		return false;
	}

	for (i = 0; i < size; i++) {
		if (file->data[i] == '\n') {
			count++;
		}
	}
	if (size > 0 && file->data[size - 1] != '\n') {
		count++;
	}
	file->lines = malloc(((size_t)count + 1) * sizeof(*file->lines));
	if (!file->lines) {
		free(file->data);
		memset(file, 0, sizeof(*file));
		return false;
	}

	start = file->data;
	for (i = 0; i < size; i++) {
		if (file->data[i] == '\n') {
			file->data[i] = '\0';
			if (i > 0 && file->data[i - 1] == '\r') {
				file->data[i - 1] = '\0';
			}
			file->lines[file->count++] = start;
			start                      = file->data + i + 1;
		}
	}
	if (file->count < count) {
		file->lines[file->count++] = start;
	}
	return true;
}

void watch_file_free(WatchFile *file) {
	free(file->lines);
	free(file->data);
	memset(file, 0, sizeof(*file));
}

int watch_find(const WatchFile *file, const Watch *watch) {
	char name[WATCH_NAME_SIZE];
	char value[WATCH_VALUE_SIZE];
	int  best = 0;
	int  i;

	if (watch->name[0] == '\0') {
		return watch->line >= 1 && watch->line <= file->count ? watch->line : 0;
	}
	for (i = 0; i < file->count; i++) {
		if (watch_parse_line(file->lines[i], name, sizeof(name), value, sizeof(value)) &&
			strcmp(name, watch->name) == 0) {
			if (!best || abs(i + 1 - watch->line) < abs(best - watch->line)) {
				best = i + 1;
			}
		}
	}
	return best;
}

int watch_list_add(WatchList *list, const char *path, const char *name, int line) {
	int    at = list->count;
	int    i;
	Watch *watch;

	if (list->count >= WATCH_MAX) {
		return -1;
	}
	/* After the file's other watches, before the first of them further down. */
	for (i = 0; i < list->count; i++) {
		if (strcmp(list->watches[i].path, path) == 0) {
			at = i + 1;
			if (list->watches[i].line > line) {
				at = i;
				break;
			}
		}
	}
	memmove(&list->watches[at + 1], &list->watches[at],
		(size_t)(list->count - at) * sizeof(list->watches[0]));
	list->count++;

	watch = &list->watches[at];
	snprintf(watch->path, sizeof(watch->path), "%s", path);
	copy_trimmed(watch->name, sizeof(watch->name), name, name + strlen(name));
	watch->line     = line;
	watch->last[0]  = '\0';
	watch->has_last = false;
	return at;
}

void watch_list_remove(WatchList *list, int index) {
	if (index < 0 || index >= list->count) {
		return;
	}
	memmove(&list->watches[index], &list->watches[index + 1],
		(size_t)(list->count - index - 1) * sizeof(list->watches[0]));
	list->count--;
}

/* The file is one entry per line with tabs between the fields:

	   jobcheck 1
	   submit <tab> sbatch job.sh
	   test <tab> sbatch --test-only job.sh
	   watch <tab> 12 <tab> --nodes <tab> job.sh
	   last <tab> 4                 the watch above, at the last submission
*/
bool watch_list_load(WatchList *list, const char *path) {
	char  line[WATCH_PATH_SIZE + WATCH_COMMAND_SIZE + WATCH_NAME_SIZE];
	FILE *stream = fopen(path, "r");

	list->command[0] = '\0';
	list->test[0]    = '\0';
	list->count      = 0;
	if (!stream) {
		return false;
	}
	while (fgets(line, sizeof(line), stream)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "submit\t", 7) == 0) {
			copy_trimmed(list->command, sizeof(list->command), line + 7, line + strlen(line));
		} else if (strncmp(line, "test\t", 5) == 0) {
			copy_trimmed(list->test, sizeof(list->test), line + 5, line + strlen(line));
		} else if (strncmp(line, "last\t", 5) == 0 && list->count > 0) {
			Watch *watch = &list->watches[list->count - 1];
			copy_trimmed(watch->last, sizeof(watch->last), line + 5, line + strlen(line));
			watch->has_last = true;
		} else if (strncmp(line, "watch\t", 6) == 0 && list->count < WATCH_MAX) {
			char  *number = line + 6;
			char  *name   = strchr(number, '\t');
			char  *file   = name ? strchr(name + 1, '\t') : NULL;
			Watch *watch  = &list->watches[list->count];
			if (!file) {
				continue;
			}
			*name++         = '\0';
			*file++         = '\0';
			watch->line     = atoi(number);
			watch->last[0]  = '\0';
			watch->has_last = false;
			copy_trimmed(watch->name, sizeof(watch->name), name, name + strlen(name));
			copy_trimmed(watch->path, sizeof(watch->path), file, file + strlen(file));
			list->count++;
		}
	}
	fclose(stream);
	return true;
}

bool watch_list_save(const WatchList *list, const char *path) {
	FILE *stream = fopen(path, "w");
	int   i;

	if (!stream) {
		return false;
	}
	fprintf(stream, "jobcheck 1\n");
	fprintf(stream, "submit\t%s\n", list->command);
	fprintf(stream, "test\t%s\n", list->test);
	for (i = 0; i < list->count; i++) {
		const Watch *watch = &list->watches[i];
		fprintf(stream, "watch\t%d\t%s\t%s\n", watch->line, watch->name, watch->path);
		if (watch->has_last) {
			fprintf(stream, "last\t%s\n", watch->last);
		}
	}
	return fclose(stream) == 0;
}
