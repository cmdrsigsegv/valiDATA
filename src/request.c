#include "request.h"

#include "watch.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The number a text starts with, or 0 when it starts with none. */
static long leading_number(const char *text) {
	char *end;
	long  value = strtol(text, &end, 10);
	return end == text || value < 0 ? 0 : value;
}

/* Reads numbers with colons between them, up to 3; returns how many. */
static int read_fields(const char *text, long *fields) {
	int count = 0;
	while (count < 3 && *text >= '0' && *text <= '9') {
		char *end;
		fields[count++] = strtol(text, &end, 10);
		if (*end != ':') {
			break;
		}
		text = end + 1;
	}
	return count;
}

/* Slurm: minutes, minutes:seconds, hours:minutes:seconds, days-hours,
   days-hours:minutes, days-hours:minutes:seconds. */
static long slurm_seconds(const char *text) {
	long        fields[3] = { 0, 0, 0 };
	const char *dash      = text + strspn(text, "0123456789");
	int         count;

	if (*dash == '-' && dash > text) {
		long days = strtol(text, NULL, 10);
		read_fields(dash + 1, fields);
		return ((days * 24 + fields[0]) * 60 + fields[1]) * 60 + fields[2];
	}
	count = read_fields(text, fields);
	switch (count) {
	case 1:
		return fields[0] * 60;
	case 2:
		return fields[0] * 60 + fields[1];
	case 3:
		return (fields[0] * 60 + fields[1]) * 60 + fields[2];
	}
	return 0;
}

/* PBS: seconds, minutes:seconds, hours:minutes:seconds. */
static long pbs_seconds(const char *text) {
	long fields[3] = { 0, 0, 0 };
	switch (read_fields(text, fields)) {
	case 1:
		return fields[0];
	case 2:
		return fields[0] * 60 + fields[1];
	case 3:
		return (fields[0] * 60 + fields[1]) * 60 + fields[2];
	}
	return 0;
}

/* nodes=2:ppn=8,walltime=01:00:00 */
static void pbs_resources(Request *request, const char *text) {
	while (*text && *text != ' ' && *text != '\t') {
		if (strncmp(text, "walltime=", 9) == 0) {
			request->seconds = pbs_seconds(text + 9);
			text += strcspn(text, ", \t");
		} else {
			if (strncmp(text, "nodes=", 6) == 0) {
				request->nodes = leading_number(text + 6);
			} else if (strncmp(text, "select=", 7) == 0) {
				request->nodes = leading_number(text + 7);
			} else if (strncmp(text, "ppn=", 4) == 0) {
				request->tasks_per_node = leading_number(text + 4);
			} else if (strncmp(text, "ncpus=", 6) == 0) {
				request->tasks_per_node = leading_number(text + 6);
			}
			text += strcspn(text, ":, \t");
		}
		if (*text == ':' || *text == ',') {
			text++;
		}
	}
}

static bool is(const char *name, const char *long_name, const char *short_name) {
	return strcmp(name, long_name) == 0 || (short_name && strcmp(name, short_name) == 0);
}

void request_line(Request *request, const char *line) {
	char name[WATCH_NAME_SIZE];
	char value[WATCH_VALUE_SIZE];
	bool slurm;

	while (*line == ' ' || *line == '\t') {
		line++;
	}
	slurm = strncmp(line, "#SBATCH", 7) == 0;
	if (!slurm && strncmp(line, "#PBS", 4) != 0) {
		return;
	}
	if (!watch_parse_line(line, name, sizeof(name), value, sizeof(value))) {
		return;
	}
	if (!slurm) {
		if (strcmp(name, "-l") == 0) {
			pbs_resources(request, value);
		}
	} else if (is(name, "--nodes", "-N")) {
		request->nodes = leading_number(value);
	} else if (is(name, "--ntasks", "-n")) {
		request->tasks = leading_number(value);
	} else if (is(name, "--ntasks-per-node", NULL)) {
		request->tasks_per_node = leading_number(value);
	} else if (is(name, "--cpus-per-task", "-c")) {
		request->cpus_per_task = leading_number(value);
	} else if (is(name, "--time", "-t")) {
		request->seconds = slurm_seconds(value);
	}
}

static void append(char *out, size_t size, const char *format, ...) {
	size_t  used = strlen(out);
	va_list arguments;
	va_start(arguments, format);
	if (used < size) {
		vsnprintf(out + used, size - used, format, arguments);
	}
	va_end(arguments);
}

static const char *plural(long count) {
	return count == 1 ? "" : "s";
}

bool request_describe(const Request *request, char *out, size_t size) {
	long cpus    = request->cpus_per_task;
	long cores   = 0;
	bool product = false; /* whether the cores are worth spelling out */

	out[0] = '\0';
	if (request->tasks > 0) {
		cores = request->tasks * (cpus > 0 ? cpus : 1);
		append(out, size, "%ld task%s", request->tasks, plural(request->tasks));
		if (request->nodes > 0) {
			append(out, size, " on %ld node%s", request->nodes, plural(request->nodes));
		}
		product = cpus > 0;
	} else if (request->tasks_per_node > 0) {
		long nodes = request->nodes > 0 ? request->nodes : 1;
		cores      = nodes * request->tasks_per_node * (cpus > 0 ? cpus : 1);
		append(
			out, size, "%ld node%s x %ld per node", nodes, plural(nodes), request->tasks_per_node);
		product = true;
	} else if (cpus > 0) {
		cores = cpus;
		append(out, size, "1 task");
		product = true;
	} else if (request->nodes > 0) {
		append(out, size, "%ld node%s", request->nodes, plural(request->nodes));
	}
	if (cpus > 0) {
		append(out, size, " x %ld cpu%s per task", cpus, plural(cpus));
	}
	if (product) {
		append(out, size, " = %ld core%s", cores, plural(cores));
	}

	if (request->seconds > 0) {
		long   s     = request->seconds;
		double hours = (double)s / 3600.0;
		append(out, size, out[0] ? " for " : "");
		if (s >= 86400) {
			append(out, size, "%ld-", s / 86400);
		}
		append(out, size, "%02ld:%02ld:%02ld", s % 86400 / 3600, s % 3600 / 60, s % 60);
		if (cores > 0) {
			append(out, size, " = %.6g core-hours", (double)cores * hours);
		} else if (request->nodes > 0) {
			append(out, size, " = %.6g node-hours", (double)request->nodes * hours);
		}
	}
	return out[0] != '\0';
}
