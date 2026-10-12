#include "request.h"
#include "watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            printf("%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void check_parse(const char *line, const char *name, const char *value)
{
    char got_name[WATCH_NAME_SIZE];
    char got_value[WATCH_VALUE_SIZE];
    bool parsed = watch_parse_line(line, got_name, sizeof(got_name), got_value, sizeof(got_value));
    if (!parsed || strcmp(got_name, name) != 0 || strcmp(got_value, value) != 0) {
        printf("parse \"%s\": got \"%s\" = \"%s\", expected \"%s\" = \"%s\"\n", line, got_name,
               got_value, name, value);
        failures++;
    }
}

static void test_parse(void)
{
    char name[WATCH_NAME_SIZE];
    char value[WATCH_VALUE_SIZE];

    check_parse("#SBATCH --nodes=4", "--nodes", "4");
    check_parse("#SBATCH --time 02:00:00", "--time", "02:00:00");
    check_parse("#SBATCH -N 4", "-N", "4");
    check_parse("#SBATCH --exclusive", "--exclusive", "");
    check_parse("#PBS -l nodes=2:ppn=8", "-l", "nodes=2:ppn=8");
    check_parse("#$ -pe mpi 16", "-pe", "mpi 16");
    check_parse("#BSUB -W 1:00", "-W", "1:00");
    check_parse("#FLUX: --nodes=2", "--nodes", "2");
    check_parse("export OMP_NUM_THREADS=8", "OMP_NUM_THREADS", "8");
    check_parse("setenv OMP_NUM_THREADS 8", "OMP_NUM_THREADS", "8");
    check_parse("NAME=\"run 12\"", "NAME", "\"run 12\"");
    check_parse("  dt = 0.01,  ! time step\r", "dt", "0.01");
    check_parse("#SBATCH --time=02:00:00   # two hours", "--time", "02:00:00");
    check_parse("steps: 1000 // short", "steps", "1000");
    check_parse("name = \"run #3 ! x\"  # the name", "name", "\"run #3 ! x\"");
    check_parse("colour = #ff0000", "colour", "#ff0000");
    check_parse("url=http://host/x#top;", "url", "http://host/x#top");
    check_parse("list = 1, 2, 3,", "list", "1, 2, 3");
    check_parse("x != 3", "x", "!= 3");
    check_parse("steps: 1000", "steps", "1000");
    check_parse("output: /scratch/a=b", "output", "/scratch/a=b");
    check_parse("nsteps 1000", "nsteps", "1000");
    check_parse("\tnsteps\t1000", "nsteps", "1000");
    check_parse("srun -n 4 ./run --dt=0.1", "srun", "-n 4 ./run --dt=0.1");
    check_parse("url=http://host:80/x", "url", "http://host:80/x");
    check_parse("#!/bin/bash", "#!/bin/bash", "");
    check_parse("# a comment", "#", "a comment");

    CHECK(!watch_parse_line("", name, sizeof(name), value, sizeof(value)));
    CHECK(!watch_parse_line("  \t\r", name, sizeof(name), value, sizeof(value)));

    /* A value longer than its buffer is cut, not written past it. */
    CHECK(watch_parse_line("x = 123456789", name, 4, value, 4));
    CHECK(strcmp(name, "x") == 0 && strcmp(value, "123") == 0);
}

static void write_file(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    fputs(text, stream);
    fclose(stream);
}

static void test_file_and_find(void)
{
    const char *path = "test_watch.tmp";
    WatchFile file;
    Watch watch = {"", "dt", 2, "", false};

    write_file(path, "a = 1\r\ndt = 0.5\n\nb = 2");
    CHECK(watch_file_load(&file, path));
    CHECK(file.count == 4);
    CHECK(strcmp(file.lines[0], "a = 1") == 0);
    CHECK(strcmp(file.lines[2], "") == 0);
    CHECK(strcmp(file.lines[3], "b = 2") == 0);
    CHECK(watch_find(&file, &watch) == 2);
    watch_file_free(&file);

    /* The line moved down, and the name is there twice: the closer one. */
    write_file(path, "dt = 9\n#\n#\n#\n#\n#\n#\ndt = 0.5\n");
    watch.line = 6;
    CHECK(watch_file_load(&file, path));
    CHECK(file.count == 8);
    CHECK(watch_find(&file, &watch) == 8);
    watch.line = 2;
    CHECK(watch_find(&file, &watch) == 1);
    snprintf(watch.name, sizeof(watch.name), "gone");
    CHECK(watch_find(&file, &watch) == 0);
    watch_file_free(&file);

    write_file(path, "");
    CHECK(watch_file_load(&file, path));
    CHECK(file.count == 0);
    watch_file_free(&file);

    remove(path);
    CHECK(!watch_file_load(&file, path));
}

static void check_set(const char *line, const char *value, const char *expected)
{
    char out[256];
    bool done = watch_line_set_value(line, value, out, sizeof(out));
    if (expected ? !done || strcmp(out, expected) != 0 : done) {
        printf("set \"%s\" to \"%s\": got \"%s\"\n", line, value, done ? out : "(refused)");
        failures++;
    }
}

static void test_set_value(void)
{
    const char *path = "test_watch.tmp";
    char out[8];
    char text[128];
    FILE *stream;
    size_t length;

    check_set("#SBATCH --nodes=4", "16", "#SBATCH --nodes=16");
    check_set("#SBATCH -N 4", "8", "#SBATCH -N 8");
    check_set("  dt = 0.01,  ! step  \r", "0.02", "  dt = 0.02,  ! step  \r");
    check_set("#SBATCH --time=02:00:00   # two hours", "1-00", "#SBATCH --time=1-00   # two hours");
    check_set("export A=1", "22", "export A=22");
    check_set("steps:\t1000", "5", "steps:\t5");
    check_set("nsteps   1000", "5", "nsteps   5");
    check_set("#SBATCH --exclusive", "x", NULL); /* nothing to replace */
    check_set("", "x", NULL);
    CHECK(!watch_line_set_value("x = 1", "123456789", out, sizeof(out)));

    /* The rest of the file is kept byte for byte, line endings included. */
    write_file(path, "a = 1\r\ndt = 0.5 ! t\r\n\nb = 2");
    CHECK(watch_file_set_value(path, 2, "0.25"));
    CHECK(watch_file_set_value(path, 4, "300"));
    CHECK(!watch_file_set_value(path, 3, "x")); /* blank */
    CHECK(!watch_file_set_value(path, 5, "x")); /* past the end */
    stream = fopen(path, "rb");
    length = fread(text, 1, sizeof(text) - 1, stream);
    text[length] = '\0';
    fclose(stream);
    CHECK(strcmp(text, "a = 1\r\ndt = 0.25 ! t\r\n\nb = 300") == 0);
    remove(path);
}

static void check_request(const char *script, const char *expected)
{
    Request request;
    char line[256];
    char out[256];
    bool known;

    memset(&request, 0, sizeof(request));
    while (*script) {
        size_t length = strcspn(script, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)length, script);
        request_line(&request, line);
        script += length + (script[length] == '\n');
    }
    known = request_describe(&request, out, sizeof(out));
    if (expected ? !known || strcmp(out, expected) != 0 : known) {
        printf("request: got \"%s\", expected \"%s\"\n", out, expected ? expected : "(nothing)");
        failures++;
    }
}

static void test_request(void)
{
    check_request("#SBATCH --nodes=4\n#SBATCH --ntasks-per-node=32\n#SBATCH --time=02:00:00",
                  "4 nodes x 32 per node = 128 cores for 02:00:00 = 256 core-hours");
    check_request("#SBATCH -N 2\n#SBATCH --ntasks-per-node 8\n#SBATCH -c 4\n#SBATCH -t 1-12",
                  "2 nodes x 8 per node x 4 cpus per task = 64 cores for 1-12:00:00 = 2304 "
                  "core-hours");
    check_request("#SBATCH --ntasks=64\n#SBATCH --time=30", "64 tasks for 00:30:00 = 32 core-hours");
    check_request("#SBATCH -n 8\n#SBATCH -N 2\n#SBATCH -c 2\n#SBATCH -t 10:30",
                  "8 tasks on 2 nodes x 2 cpus per task = 16 cores for 00:10:30 = 2.8 core-hours");
    check_request("#SBATCH --nodes=3\n#SBATCH --time=0-01:30:00",
                  "3 nodes for 01:30:00 = 4.5 node-hours");
    check_request("#SBATCH --cpus-per-task=1", "1 task x 1 cpu per task = 1 core");
    check_request("#PBS -N myjob\n#PBS -l nodes=2:ppn=8,walltime=01:00:00",
                  "2 nodes x 8 per node = 16 cores for 01:00:00 = 16 core-hours");
    check_request("#PBS -l select=1:ncpus=4:mpiprocs=4\n#PBS -l walltime=00:30:00",
                  "1 node x 4 per node = 4 cores for 00:30:00 = 2 core-hours");
    check_request("#SBATCH --job-name=x\nexport N=4\nnodes = 4", NULL);
}

static void test_list(void)
{
    const char *path = "test_watch.tmp";
    static WatchList list;
    static WatchList loaded;
    int i;

    snprintf(list.command, sizeof(list.command), "sbatch job.sh --export=A=1");
    CHECK(watch_list_add(&list, "job.sh", "--time", 5) == 0);
    CHECK(watch_list_add(&list, "in put.nml", "dt", 3) == 1);
    CHECK(watch_list_add(&list, "job.sh", "--nodes", 2) == 0); /* before --time */
    CHECK(watch_list_add(&list, "job.sh", "srun", 9) == 2);    /* after it, with its file */
    CHECK(list.count == 4);
    snprintf(list.test, sizeof(list.test), "sbatch --test-only job.sh");
    snprintf(list.watches[1].last, sizeof(list.watches[1].last), "02:00:00  # two\thours");
    list.watches[1].has_last = true;
    CHECK(strcmp(list.watches[3].path, "in put.nml") == 0);

    CHECK(watch_list_save(&list, path));
    CHECK(watch_list_load(&loaded, path));
    CHECK(loaded.count == 4);
    CHECK(strcmp(loaded.command, list.command) == 0);
    CHECK(strcmp(loaded.test, list.test) == 0);
    CHECK(loaded.watches[1].has_last && !loaded.watches[0].has_last && !loaded.watches[2].has_last);
    CHECK(strcmp(loaded.watches[1].last, list.watches[1].last) == 0);
    for (i = 0; i < 4; i++) {
        CHECK(strcmp(loaded.watches[i].path, list.watches[i].path) == 0);
        CHECK(strcmp(loaded.watches[i].name, list.watches[i].name) == 0);
        CHECK(loaded.watches[i].line == list.watches[i].line);
    }

    watch_list_remove(&loaded, 1);
    CHECK(loaded.count == 3);
    CHECK(strcmp(loaded.watches[1].name, "srun") == 0);

    remove(path);
    CHECK(!watch_list_load(&loaded, path));
    CHECK(loaded.count == 0);
}

int main(void)
{
    test_parse();
    test_file_and_find();
    test_set_value();
    test_request();
    test_list();
    if (failures) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
