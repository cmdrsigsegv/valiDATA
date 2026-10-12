/* The part of jobcheck that knows nothing about the terminal: which lines of
   which files are watched, how a line is read as a name and a value, and the
   file the list is kept in. */

#ifndef WATCH_H
#define WATCH_H

#include <stdbool.h>
#include <stddef.h>

#define WATCH_NAME_SIZE    128
#define WATCH_VALUE_SIZE   512
#define WATCH_PATH_SIZE    1024
#define WATCH_COMMAND_SIZE 1024
#define WATCH_MAX          256

typedef struct {
	char path[WATCH_PATH_SIZE];  /* as it was picked, relative to the directory jobcheck runs in */
	char name[WATCH_NAME_SIZE];  /* what the line sets; the line is found again by it */
	int  line;                   /* where it was last seen, from 1 */
	char last[WATCH_VALUE_SIZE]; /* the value at the last submission that succeeded */
	bool has_last;               /* false until there was one */
} Watch;

typedef struct {
	char  command[WATCH_COMMAND_SIZE]; /* run by /bin/sh once everything is ticked */
	char  test[WATCH_COMMAND_SIZE];    /* the dry run, such as sbatch --test-only job.sh */
	Watch watches[WATCH_MAX];          /* those of one file follow each other, by line */
	int   count;
} WatchList;

/* A text file in memory, cut into lines. */
typedef struct {
	char  *data;
	char **lines;
	int    count;
} WatchFile;

/* Reads a line as a name and a value:

	   #SBATCH --nodes=4        --nodes    4
	   #PBS -l walltime=1:00    -l         walltime=1:00
	   export OMP_NUM_THREADS=8 OMP_NUM_THREADS  8
	   dt = 0.01,  ! time step  dt         0.01
	   steps: 1000   # short    steps      1000
	   srun -n 4 ./run          srun       -n 4 ./run

   The value ends before a comment (a #, ! or // after whitespace, outside
   quotes) and before a comma or semicolon that closes it.
   False for a line that is blank. */
bool watch_parse_line(
	const char *line, char *name, size_t name_size, char *value, size_t value_size);

/* Writes the line again with another value in place of its own; what is before
   and after the value, a comment for one, stays as it is. False for a line
   without a value, and when the result does not fit. */
bool watch_line_set_value(const char *line, const char *value, char *out, size_t size);

/* The same for a line of a file, from 1; the rest of the file is written back
   byte for byte. */
bool watch_file_set_value(const char *path, int line, const char *value);

bool watch_file_load(WatchFile *file, const char *path);
void watch_file_free(WatchFile *file);

/* The line the watch is on now, from 1, or 0 when no line sets its name any
   more. Of several lines with the name, the one closest to where it was. */
int watch_find(const WatchFile *file, const Watch *watch);

/* The index it was put at, or -1 when the list is full. */
int  watch_list_add(WatchList *list, const char *path, const char *name, int line);
void watch_list_remove(WatchList *list, int index);

/* False when the file cannot be read; the list is empty then. */
bool watch_list_load(WatchList *list, const char *path);
bool watch_list_save(const WatchList *list, const char *path);

#endif
