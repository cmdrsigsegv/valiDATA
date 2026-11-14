# valiDATA

A last look at a job before it goes to the queue.

valiDATA is a small terminal program, `jobcheck`, for people who submit
simulations to a cluster. You tell it which lines of your job script and
input files matter: the number of nodes, the wall time, the time step, the
output directory. Before every submission it shows you the current value of
each of those lines, and it runs your submit command only after you have
ticked every one of them.

It does not decide whether a value is right. It makes sure you looked.

```
 jobcheck   tick every value to submit
 submit    sbatch job.sh
 request   4 nodes x 32 per node = 128 cores for 02:00:00 = 256 core-hours

 job.sh
   [x] --nodes            4  line 3
   [x] --ntasks-per-node  32  line 4
   [ ] --time             02:00:00   was 01:00:00  line 5
 input.par
   [ ] dt                 0.005   edited, file has 0.01  line 12

 2 of 4 checked   1 not as last submitted   1 edited, written on submit
 space tick  e edit  enter submit  t test  r reread  h history  w list  q quit
```

## Why

Most wasted allocations come from a value nobody looked at: yesterday's test
resolution, a wall time of ten minutes, an output path that overwrites the
last run. The values are all in plain sight in the files, but there are many
files and the submit command is one line. valiDATA puts the few values that
matter on one screen and stands between you and `sbatch` until you have
confirmed each one.

## What it does

- **Shows the watched values** of any text file: job scripts, namelists,
  parameter files, shell scripts.
- **Submits only when everything is ticked.** A value that changes in its
  file after you ticked it loses the tick.
- **Marks what is different from last time.** Each value that is not what
  the last successful submission had is shown with `was ...`.
- **Adds up the request.** From the `#SBATCH` and `#PBS` lines it works out
  the cores, the wall time and the core-hours you are about to ask for.
- **Lets you change a value on the spot.** The change is written to the file
  at the moment of submission and not before, and is taken out again if the
  submit command fails.
- **Keeps a history.** Every submission is logged with the time, the
  command, every watched value and what the scheduler answered.

## Install

valiDATA needs a C99 compiler and `make` on Linux or another POSIX system.
It has no other dependencies and does not use curses, so it builds on a
cluster login node as it is. On Windows, build it in WSL.

```sh
git clone https://github.com/cmdrsigsegv/valiDATA.git
cd valiDATA
make            # builds ./jobcheck
make test       # runs the tests
make install    # copies jobcheck to ~/.local/bin
```

`make install PREFIX=/some/where` installs to `/some/where/bin` instead.

## First use

Run `jobcheck` in the directory you submit from. The first time there is no
list yet, so it opens the list editor.

1. Press `a` and pick a file. The arrow keys move, `Enter` opens a file or a
   directory, the left arrow goes up a directory.
2. In the file, move to a line you want to watch and press `Enter`. A `*`
   marks watched lines. The bottom of the screen shows how the line is read,
   as a name and a value. Press `Esc` to go back and pick lines in other
   files.
3. Press `c` and type the submit command, for example `sbatch job.sh`.
4. Optionally press `t` to set a dry-run command. For an `sbatch` command it
   suggests `sbatch --test-only ...`.
5. Press `Enter` to leave the editor.

The list is saved in `.jobcheck` in that directory. From then on, running
`jobcheck` goes straight to the check.

## Every submission

Run `jobcheck` in place of your submit command.

| Key | What it does |
| --- | --- |
| arrows, `j` `k`, PgUp PgDn, Home End | move |
| `Space` or `x` | tick or untick the value |
| `e` | type a new value over the current one |
| `Enter` | submit, once every value is ticked |
| `t` | run the dry-run command and come back |
| `r` | read the files again |
| `h` | show the history of submissions |
| `w` | edit the list of watched values |
| `q` | quit without submitting |

When you press `Enter`, `jobcheck` reads the files once more. If a value
changed in the meantime, if a watched line has disappeared, or if anything is
still unticked, it says so and does not submit. Otherwise it writes the
values you edited into their files, runs the submit command, and adds the
job to the history.

In the list editor (`w`): `a` adds lines, `d` removes the selected one, `c`
sets the submit command, `t` sets the dry-run command, `Enter` goes back.

## How a line is read

A watched line is read as a name and a value. The name is how the line is
found again when the file is edited and lines move.

| Line in the file | Name | Value |
| --- | --- | --- |
| `#SBATCH --nodes=4` | `--nodes` | `4` |
| `#PBS -l walltime=1:00` | `-l` | `walltime=1:00` |
| `export OMP_NUM_THREADS=8` | `OMP_NUM_THREADS` | `8` |
| `dt = 0.01,  ! time step` | `dt` | `0.01` |
| `steps: 1000   # short` | `steps` | `1000` |
| `srun -n 4 ./run` | `srun` | `-n 4 ./run` |

A comment after the value (`#`, `!` or `//` after a space) and a closing
comma or semicolon are not part of the value, and they are left as they are
when a value is edited. Directives of other schedulers (`#$`, `#BSUB`,
`#FLUX:`) are read in the same way as `#SBATCH`.

If no line sets a watched name any more, the value is shown as
`[!] not found in the file` and nothing can be submitted until the list is
fixed.

## The request line

For Slurm and PBS scripts, `jobcheck` adds up what the watched files ask for
and shows it under the submit command, using the values on the screen, edits
included:

```
request   4 nodes x 32 per node = 128 cores for 02:00:00 = 256 core-hours
```

It understands `--nodes`/`-N`, `--ntasks`/`-n`, `--ntasks-per-node`,
`--cpus-per-task`/`-c` and `--time`/`-t` on `#SBATCH` lines, and
`nodes=`, `ppn=`, `select=`, `ncpus=` and `walltime=` on `#PBS -l` lines.
A file is counted only if at least one of its lines is watched.

## The history

Every submission is appended to `.jobcheck.history`, next to the list:

```
2026-11-14 10:32:07   submitted
command: sbatch job.sh
job.sh
    --nodes            4
    --ntasks-per-node  32
    --time             02:00:00   (was 01:00:00)
input.par
    dt                 0.005   (was 0.01)
request: 4 nodes x 32 per node = 128 cores for 02:00:00 = 256 core-hours
output:
    Submitted batch job 123456
```

A failed submission is logged as `FAILED` with the exit status. Press `h`
to read the history inside `jobcheck`.

## Options and files

```
jobcheck [edit] [-f LIST]

  edit      start in the list editor
  -f LIST   keep the list in LIST instead of .jobcheck
```

- `.jobcheck` holds the commands and the watched lines, with the values of
  the last successful submission. It is a small tab-separated text file.
  Paths in it are relative to the directory `jobcheck` runs in.
- `.jobcheck.history` is the log. For a list named `runs/a.list` it is
  `runs/.a.list.history`.
- Several lists can live in one directory: `jobcheck -f production.list`.
- Colours are switched off when the `NO_COLOR` environment variable is set.

`jobcheck` exits with the status of the submit command, with 1 when you quit
without submitting, and with 2 when it is not run in a terminal or is given
an option it does not know. A script can therefore tell whether the job went
in.

## Limits

- At most 256 watched values per list, and values of at most 511 characters.
- Files larger than 16 MB are not read.
- It needs an interactive terminal. In a pipe or a batch script it refuses
  to submit.

## Citing

If valiDATA is useful in your work, please cite the archived release. This
DOI always points to the latest version:

> Els, P.L. (2026). valiDATA: job validation tool pre-submission.
> Zenodo. https://doi.org/10.5281/zenodo.24228554

## Licence

Apache License 2.0. See `LICENSE`.

## Author

Paul L. Els, Centre for mathematical Plasma Astrophysics, KU Leuven.
