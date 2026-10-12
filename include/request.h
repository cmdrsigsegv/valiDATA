/* What a job script asks the scheduler for, added up from its #SBATCH and
   #PBS lines: cores, wall time, and the core-hours those make. */

#ifndef REQUEST_H
#define REQUEST_H

#include <stdbool.h>
#include <stddef.h>

/* 0 where the script does not say. */
typedef struct {
	long nodes;
	long tasks;          /* in all: --ntasks */
	long tasks_per_node; /* --ntasks-per-node, ppn, ncpus of a chunk */
	long cpus_per_task;
	long seconds;
} Request;

/* Takes what a line asks for; lines that are not directives are passed over.
   Start from a Request of zeros.

	   #SBATCH --nodes -N, --ntasks -n, --ntasks-per-node, --cpus-per-task -c, --time -t
	   #PBS -l nodes=2:ppn=8,walltime=01:00:00     #PBS -l select=2:ncpus=8 */
void request_line(Request *request, const char *line);

/* "4 nodes x 32 per node = 128 cores for 02:00:00 = 256 core-hours".
   False when the lines asked for nothing that is known here. */
bool request_describe(const Request *request, char *out, size_t size);

#endif
