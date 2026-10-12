/* jobcheck: shows the watched values of a job's files and runs the submit
   command only once every one of them has been ticked.

   The terminal is driven with escape codes and termios alone, so that it
   builds on a cluster without curses. */

#define _XOPEN_SOURCE 700

#include "request.h"
#include "watch.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RESTORE "\x1b[0m\x1b[?25h\x1b[?1049l"

#define BOLD "\x1b[1m"
#define DIM "\x1b[2m"
#define REVERSE "\x1b[7m"
#define PLAIN "\x1b[0m"

enum {
    KEY_ENTER = '\r',
    KEY_ESCAPE = 27,
    KEY_CTRL_C = 3,
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_RESIZE
};

/* What was read of a watch at the last look at its file. */
typedef struct {
    char value[WATCH_VALUE_SIZE];  /* as it is in the file */
    char edited[WATCH_VALUE_SIZE]; /* typed over it here; goes into the file at submission */
    char old[WATCH_VALUE_SIZE];    /* what the file had before `edited` was written into it */
    bool has_edit;
    bool written;
    bool found;
    bool ticked;
} Item;

typedef struct {
    char name[256];
    bool directory;
} Entry;

static struct termios old_termios;
static bool terminal_taken;
static volatile sig_atomic_t resized;
static int rows = 24;
static int columns = 80;
static const char *green = "";
static const char *red = "";
static const char *yellow = "";

static WatchList list;
static Item items[WATCH_MAX];
static const char *list_path = ".jobcheck";
static char message[256];
static char request_text[256]; /* what the job scripts ask for in all, or empty */
static char browse_directory[WATCH_PATH_SIZE] = ".";

/* ---- terminal ---- */

static void terminal_restore(void)
{
    if (terminal_taken) {
        fputs(RESTORE, stdout);
        fflush(stdout);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
        terminal_taken = false;
    }
}

static void on_terminate(int number)
{
    if (terminal_taken) {
        if (write(STDOUT_FILENO, RESTORE, sizeof(RESTORE) - 1) < 0) {
            /* nothing left to do about it */
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
    }
    _exit(128 + number);
}

static void on_resize(int number)
{
    (void)number;
    resized = 1;
}

static void terminal_take(void)
{
    static bool prepared;
    struct termios raw;
    struct sigaction action;

    tcgetattr(STDIN_FILENO, &old_termios);
    raw = old_termios;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    terminal_taken = true;

    if (!prepared) {
        prepared = true;
        atexit(terminal_restore);

        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        action.sa_handler = on_terminate;
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
#ifdef SIGWINCH
        action.sa_handler = on_resize; /* without SA_RESTART, so that it ends the read */
        sigaction(SIGWINCH, &action, NULL);
#endif
        setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    }
    fputs("\x1b[?1049h\x1b[?25l", stdout);
}

static int read_key(void)
{
    for (;;) {
        unsigned char c;
        ssize_t got;

        if (resized) {
            resized = 0;
            return KEY_RESIZE;
        }
        got = read(STDIN_FILENO, &c, 1);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            exit(1); /* the terminal is gone */
        }
        if (c == 27) {
            struct pollfd input = {STDIN_FILENO, POLLIN, 0};
            int number = 0;
            if (poll(&input, 1, 50) <= 0) {
                return KEY_ESCAPE;
            }
            if (read(STDIN_FILENO, &c, 1) != 1 || (c != '[' && c != 'O')) {
                return 0;
            }
            for (;;) {
                if (read(STDIN_FILENO, &c, 1) != 1) {
                    return 0;
                }
                if (c >= '0' && c <= '9') {
                    number = number * 10 + (c - '0');
                } else if (c >= 0x40 && c <= 0x7e) {
                    break;
                }
            }
            switch (c) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            case '~':
                switch (number) {
                case 1: case 7: return KEY_HOME;
                case 4: case 8: return KEY_END;
                case 3: return KEY_DELETE;
                case 5: return KEY_PAGE_UP;
                case 6: return KEY_PAGE_DOWN;
                }
            }
            return 0;
        }
        if (c == '\n') {
            return KEY_ENTER;
        }
        if (c == 127 || c == 8) {
            return KEY_BACKSPACE;
        }
        return c;
    }
}

/* ---- drawing ---- */

static int lines_drawn;

static void frame_begin(void)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0 && size.ws_col > 0) {
        rows = size.ws_row;
        columns = size.ws_col;
    }
    fputs("\x1b[H", stdout);
    lines_drawn = 0;
}

static void line_end(void)
{
    fputs(PLAIN "\x1b[K", stdout);
    lines_drawn++;
    if (lines_drawn < rows) {
        fputs("\r\n", stdout);
    }
}

static void frame_end(void)
{
    fputs("\x1b[J", stdout);
    fflush(stdout);
}

/* Prints at most `width` columns of UTF-8 text and returns how many it took.
   Tabs go to the next multiple of `tab` columns and other control characters
   become question marks. */
static int put_text_tabs(const char *text, int width, int tab)
{
    int used = 0;
    while (*text && used < width) {
        unsigned char c = (unsigned char)*text++;
        if (c == '\t') {
            do {
                putchar(' ');
                used++;
            } while (used % tab != 0 && used < width);
        } else if (c < 32 || c == 127) {
            putchar('?');
            used++;
        } else {
            putchar(c);
            while (((unsigned char)*text & 0xc0) == 0x80) {
                putchar(*text++);
            }
            used++;
        }
    }
    return used;
}

static int put_text(const char *text, int width)
{
    return put_text_tabs(text, width, 1);
}

static void put_spaces(int count)
{
    while (count-- > 0) {
        putchar(' ');
    }
}

static int text_columns(const char *text)
{
    int count = 0;
    for (; *text; text++) {
        if (((unsigned char)*text & 0xc0) != 0x80) {
            count++;
        }
    }
    return count;
}

/* Moves the cursor of a list by a key; false when the key is not one of them. */
static bool move_cursor(int key, int *cursor, int count, int page)
{
    switch (key) {
    case KEY_UP: case 'k': (*cursor)--; break;
    case KEY_DOWN: case 'j': (*cursor)++; break;
    case KEY_PAGE_UP: *cursor -= page; break;
    case KEY_PAGE_DOWN: *cursor += page; break;
    case KEY_HOME: case 'g': *cursor = 0; break;
    case KEY_END: case 'G': *cursor = count - 1; break;
    default: return false;
    }
    if (*cursor >= count) {
        *cursor = count - 1;
    }
    if (*cursor < 0) {
        *cursor = 0;
    }
    return true;
}

static void scroll_to(int *top, int cursor, int height)
{
    if (cursor < *top) {
        *top = cursor;
    }
    if (cursor >= *top + height) {
        *top = cursor - height + 1;
    }
    if (*top < 0) {
        *top = 0;
    }
}

static bool continues(char c)
{
    return ((unsigned char)c & 0xc0) == 0x80; /* a byte inside a UTF-8 character */
}

/* Edits a line of text on the last row. False when it was cancelled. */
static bool prompt(const char *label, char *text, size_t size)
{
    char edited[WATCH_COMMAND_SIZE];
    size_t length;
    size_t cursor;

    if (size > sizeof(edited)) {
        size = sizeof(edited);
    }
    snprintf(edited, size, "%s", text);
    length = strlen(edited);
    cursor = length;
    for (;;) {
        int room = columns - text_columns(label) - 2;
        const char *shown = edited;
        int before = 0;
        int skipped;
        int key;
        size_t i;

        for (i = 0; i < cursor; i++) {
            before += !continues(edited[i]);
        }
        if (room < 1) {
            room = 1;
        }
        skipped = before >= room ? before - room + 1 : 0; /* the cursor stays in sight */
        for (i = 0; i < (size_t)skipped && *shown; i++) {
            shown++;
            while (continues(*shown)) {
                shown++;
            }
        }
        printf("\x1b[%d;1H" PLAIN "\x1b[K" BOLD "%s" PLAIN " ", rows, label);
        put_text(shown, room);
        printf("\x1b[%d;%dH\x1b[?25h", rows, text_columns(label) + 2 + before - skipped);
        fflush(stdout);

        key = read_key();
        if (key == KEY_ENTER || key == KEY_ESCAPE || key == KEY_CTRL_C) {
            fputs("\x1b[?25l", stdout);
            if (key != KEY_ENTER) {
                return false;
            }
            snprintf(text, size, "%s", edited);
            return true;
        }
        if (key == KEY_LEFT && cursor > 0) {
            do {
                cursor--;
            } while (cursor > 0 && continues(edited[cursor]));
        } else if (key == KEY_RIGHT && cursor < length) {
            do {
                cursor++;
            } while (cursor < length && continues(edited[cursor]));
        } else if (key == KEY_HOME) {
            cursor = 0;
        } else if (key == KEY_END) {
            cursor = length;
        } else if (key == 21) { /* Ctrl-U */
            length = cursor = 0;
            edited[0] = '\0';
        } else if (key == KEY_BACKSPACE && cursor > 0) {
            size_t start = cursor - 1;
            while (start > 0 && continues(edited[start])) {
                start--;
            }
            memmove(edited + start, edited + cursor, length - cursor + 1);
            length -= cursor - start;
            cursor = start;
        } else if (key == KEY_DELETE && cursor < length) {
            size_t end = cursor + 1;
            while (end < length && continues(edited[end])) {
                end++;
            }
            memmove(edited + cursor, edited + end, length - end + 1);
            length -= end - cursor;
        } else if (key >= 32 && key < 256 && key != 127 && length + 1 < size) {
            memmove(edited + cursor + 1, edited + cursor, length - cursor + 1);
            edited[cursor++] = (char)key;
            length++;
        }
    }
}

/* ---- the watched values ---- */

/* The value the job would be submitted with. */
static const char *effective(int index)
{
    return items[index].has_edit ? items[index].edited : items[index].value;
}

/* Adds up what the #SBATCH and #PBS lines of the watched files ask for, with
   the values typed here in place of those in the files. */
static void update_request(void)
{
    Request request;
    int first = 0;

    memset(&request, 0, sizeof(request));
    while (first < list.count) {
        WatchFile file;
        int end = first + 1;
        int i;
        int j;

        while (end < list.count &&
               strcmp(list.watches[end].path, list.watches[first].path) == 0) {
            end++;
        }
        if (watch_file_load(&file, list.watches[first].path)) {
            for (i = 0; i < file.count; i++) {
                char changed[4096];
                const char *line = file.lines[i];
                for (j = first; j < end; j++) {
                    if (items[j].found && items[j].has_edit && list.watches[j].line == i + 1 &&
                        watch_line_set_value(line, items[j].edited, changed, sizeof(changed))) {
                        line = changed;
                    }
                }
                request_line(&request, line);
            }
            watch_file_free(&file);
        }
        first = end;
    }
    if (!request_describe(&request, request_text, sizeof(request_text))) {
        request_text[0] = '\0';
    }
}

/* Reads every watched value from its file again. A value that is not what it
   was loses its tick; returns how many those were. */
static int resolve(void)
{
    WatchFile file;
    const char *loaded_path = NULL;
    bool loaded = false;
    int changed = 0;
    int i;

    for (i = 0; i < list.count; i++) {
        Watch *watch = &list.watches[i];
        Item *item = &items[i];
        char old_value[WATCH_VALUE_SIZE];
        bool old_found = item->found;
        int line = 0;

        memcpy(old_value, item->value, sizeof(old_value));
        if (!loaded_path || strcmp(loaded_path, watch->path) != 0) {
            if (loaded) {
                watch_file_free(&file);
            }
            loaded = watch_file_load(&file, watch->path);
            loaded_path = watch->path;
        }
        if (loaded) {
            line = watch_find(&file, watch);
        }
        item->found = line > 0;
        item->value[0] = '\0';
        if (line > 0) {
            char name[WATCH_NAME_SIZE];
            watch_parse_line(file.lines[line - 1], name, sizeof(name), item->value,
                             sizeof(item->value));
            watch->line = line;
        }
        if (item->found != old_found || strcmp(item->value, old_value) != 0) {
            item->ticked = false;
            changed++;
        }
    }
    if (loaded) {
        watch_file_free(&file);
    }
    update_request();
    return changed;
}

/* The list and what is known of its values are kept side by side. */
static int add_watch(const char *path, const char *name, int line)
{
    int at = watch_list_add(&list, path, name, line);
    if (at >= 0) {
        memmove(&items[at + 1], &items[at], (size_t)(list.count - 1 - at) * sizeof(items[0]));
        memset(&items[at], 0, sizeof(items[at]));
    }
    return at;
}

static void remove_watch(int index)
{
    if (index < 0 || index >= list.count) {
        return;
    }
    memmove(&items[index], &items[index + 1],
            (size_t)(list.count - index - 1) * sizeof(items[0]));
    watch_list_remove(&list, index);
    memset(&items[list.count], 0, sizeof(items[0]));
}

static void save(void)
{
    if (!watch_list_save(&list, list_path)) {
        snprintf(message, sizeof(message), "Could not write %.150s: %s", list_path,
                 strerror(errno));
    }
}

/* ---- picking a line of a file ---- */

static void mark_watched(const WatchFile *file, const char *path, int *watch_of_line)
{
    int i;
    for (i = 0; i < file->count; i++) {
        watch_of_line[i] = -1;
    }
    for (i = 0; i < list.count; i++) {
        if (strcmp(list.watches[i].path, path) == 0) {
            int line = watch_find(file, &list.watches[i]);
            if (line > 0) {
                watch_of_line[line - 1] = i;
            }
        }
    }
}

static void pick_lines(const char *path)
{
    WatchFile file;
    int *watch_of_line;
    int cursor = 0;
    int top = 0;
    int digits = 1;
    int n;

    if (!watch_file_load(&file, path)) {
        snprintf(message, sizeof(message), "Could not read %.200s", path);
        return;
    }
    if (file.count == 0) {
        snprintf(message, sizeof(message), "%.200s is empty", path);
        watch_file_free(&file);
        return;
    }
    watch_of_line = malloc((size_t)file.count * sizeof(*watch_of_line));
    if (!watch_of_line) {
        watch_file_free(&file);
        return;
    }
    mark_watched(&file, path, watch_of_line);
    for (n = file.count; n >= 10; n /= 10) {
        digits++;
    }

    for (;;) {
        char name[WATCH_NAME_SIZE];
        char value[WATCH_VALUE_SIZE];
        bool parsed;
        int height;
        int key;
        int i;

        frame_begin();
        height = rows - 4 > 1 ? rows - 4 : 1;
        scroll_to(&top, cursor, height);

        fputs(BOLD " Pick the lines to watch" PLAIN "   ", stdout);
        put_text(path, columns - 28);
        line_end();
        line_end();
        for (i = top; i < top + height; i++) {
            if (i < file.count) {
                if (i == cursor) {
                    fputs(REVERSE, stdout);
                }
                printf(" %*d %s%s", digits, i + 1, watch_of_line[i] >= 0 ? green : "",
                       watch_of_line[i] >= 0 ? "*" : " ");
                if (watch_of_line[i] >= 0 && *green) {
                    fputs(i == cursor ? PLAIN REVERSE : PLAIN, stdout);
                }
                putchar(' ');
                n = put_text_tabs(file.lines[i], columns - digits - 5, 4);
                if (i == cursor) {
                    put_spaces(columns - digits - 5 - n);
                }
            }
            line_end();
        }
        parsed = watch_parse_line(file.lines[cursor], name, sizeof(name), value, sizeof(value));
        if (message[0]) {
            putchar(' ');
            put_text(message, columns - 2);
        } else if (parsed) {
            fputs(" read as  " BOLD, stdout);
            n = put_text(name, columns - 12);
            fputs(PLAIN "  =  ", stdout);
            put_text(value[0] ? value : "(no value)", columns - 17 - n);
        } else {
            fputs(DIM " blank line" PLAIN, stdout);
        }
        line_end();
        fputs(DIM, stdout);
        put_text(" arrows move   enter watch or unwatch (*)   esc back", columns - 1);
        line_end();
        frame_end();

        key = read_key();
        message[0] = '\0';
        if (move_cursor(key, &cursor, file.count, height)) {
            continue;
        }
        if (key == KEY_ENTER || key == ' ') {
            if (watch_of_line[cursor] >= 0) {
                remove_watch(watch_of_line[cursor]);
            } else if (!parsed) {
                snprintf(message, sizeof(message), "A blank line has nothing to watch.");
                continue;
            } else if (add_watch(path, name, cursor + 1) < 0) {
                snprintf(message, sizeof(message), "No room for more than %d watches.", WATCH_MAX);
                continue;
            }
            save();
            mark_watched(&file, path, watch_of_line);
        } else if (key == KEY_ESCAPE || key == KEY_LEFT || key == 'q' || key == KEY_CTRL_C) {
            break;
        }
    }
    free(watch_of_line);
    watch_file_free(&file);
}

/* ---- picking a file ---- */

static void path_join(char *out, size_t size, const char *directory, const char *name)
{
    size_t length = strlen(directory);
    if (strcmp(directory, ".") == 0) {
        snprintf(out, size, "%s", name);
    } else if (length > 0 && directory[length - 1] == '/') {
        snprintf(out, size, "%s%s", directory, name);
    } else {
        snprintf(out, size, "%s/%s", directory, name);
    }
}

static void path_parent(char *path, size_t size)
{
    char *slash = strrchr(path, '/');
    const char *last = slash ? slash + 1 : path;
    size_t length = strlen(path);

    if (strcmp(path, ".") == 0) {
        snprintf(path, size, "..");
    } else if (strcmp(path, "/") == 0) {
        /* stays */
    } else if (strcmp(last, "..") == 0) {
        if (length + 4 <= size) {
            memcpy(path + length, "/..", 4);
        }
    } else if (slash == path) {
        path[1] = '\0';
    } else if (slash) {
        *slash = '\0';
    } else {
        snprintf(path, size, ".");
    }
}

static int compare_entries(const void *a, const void *b)
{
    const Entry *x = a;
    const Entry *y = b;
    if (x->directory != y->directory) {
        return x->directory ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

/* The entries of a directory, its directories first. NULL when it cannot be read. */
static Entry *read_directory(const char *path, int *count)
{
    DIR *directory = opendir(path);
    struct dirent *found;
    Entry *entries = NULL;
    int room = 0;

    *count = 0;
    if (!directory) {
        return NULL;
    }
    while ((found = readdir(directory)) != NULL) {
        char full[WATCH_PATH_SIZE + 256];
        struct stat info;
        if (strcmp(found->d_name, ".") == 0 || strcmp(found->d_name, "..") == 0) {
            continue;
        }
        if (*count == room) {
            Entry *grown = realloc(entries, (size_t)(room ? room * 2 : 64) * sizeof(*entries));
            if (!grown) {
                break;
            }
            entries = grown;
            room = room ? room * 2 : 64;
        }
        snprintf(entries[*count].name, sizeof(entries[*count].name), "%s", found->d_name);
        path_join(full, sizeof(full), path, found->d_name);
        entries[*count].directory = stat(full, &info) == 0 && S_ISDIR(info.st_mode);
        (*count)++;
    }
    closedir(directory);
    if (!entries) {
        entries = malloc(sizeof(*entries)); /* an empty directory is not a failure */
    }
    if (entries) {
        qsort(entries, (size_t)*count, sizeof(*entries), compare_entries);
    }
    return entries;
}

static void browse(void)
{
    Entry *entries = NULL;
    int count = 0;
    int cursor = 0;
    int top = 0;
    bool reload = true;

    for (;;) {
        int height;
        int key;
        int i;

        if (reload) {
            free(entries);
            entries = read_directory(browse_directory, &count);
            if (!entries) {
                snprintf(message, sizeof(message), "Could not open %.200s", browse_directory);
                snprintf(browse_directory, sizeof(browse_directory), ".");
                entries = read_directory(browse_directory, &count);
                if (!entries) {
                    return;
                }
            }
            cursor = 0;
            top = 0;
            reload = false;
        }

        frame_begin();
        height = rows - 4 > 1 ? rows - 4 : 1;
        scroll_to(&top, cursor, height);

        fputs(BOLD " Pick a file" PLAIN "   ", stdout);
        put_text(browse_directory, columns - 16);
        putchar('/');
        line_end();
        line_end();
        for (i = top; i < top + height; i++) {
            if (i < count) {
                int n;
                fputs(i == cursor ? REVERSE "  " : "  ", stdout);
                if (entries[i].directory) {
                    fputs(BOLD, stdout);
                }
                n = put_text(entries[i].name, columns - 4);
                if (entries[i].directory) {
                    putchar('/');
                    n++;
                }
                if (i == cursor) {
                    put_spaces(columns - 2 - n);
                }
            } else if (count == 0 && i == top) {
                fputs(DIM "  (empty)" PLAIN, stdout);
            }
            line_end();
        }
        putchar(' ');
        put_text(message, columns - 2);
        line_end();
        fputs(DIM, stdout);
        put_text(" arrows move   enter open   left parent directory   esc back", columns - 1);
        line_end();
        frame_end();

        key = read_key();
        message[0] = '\0';
        if (move_cursor(key, &cursor, count, height)) {
            continue;
        }
        if ((key == KEY_ENTER || key == KEY_RIGHT || key == 'l') && count > 0) {
            char path[WATCH_PATH_SIZE];
            path_join(path, sizeof(path), browse_directory, entries[cursor].name);
            if (entries[cursor].directory) {
                snprintf(browse_directory, sizeof(browse_directory), "%s", path);
                reload = true;
            } else {
                pick_lines(path);
            }
        } else if (key == KEY_LEFT || key == KEY_BACKSPACE || key == 'h') {
            path_parent(browse_directory, sizeof(browse_directory));
            reload = true;
        } else if (key == KEY_ESCAPE || key == 'q' || key == KEY_CTRL_C) {
            break;
        }
    }
    free(entries);
}

/* ---- submitting ---- */

static int exit_status(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/* Runs a command with /bin/sh. What it prints goes to the terminal and, as
   much as fits, into `output`. Returns its exit status. */
static int run_command(const char *command, char *output, size_t size)
{
    struct sigaction ignore;
    struct sigaction old_interrupt;
    char buffer[4096];
    size_t used = 0;
    int ends[2];
    int status = 0;
    ssize_t got;
    pid_t child;

    output[0] = '\0';
    if (pipe(ends) != 0) {
        return 127;
    }
    /* Ctrl-C is for the command; this program still has the history to write. */
    memset(&ignore, 0, sizeof(ignore));
    sigemptyset(&ignore.sa_mask);
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ignore, &old_interrupt);

    child = fork();
    if (child == 0) {
        sigaction(SIGINT, &old_interrupt, NULL);
        dup2(ends[1], STDOUT_FILENO);
        dup2(ends[1], STDERR_FILENO);
        close(ends[0]);
        close(ends[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(ends[1]);
    if (child < 0) {
        close(ends[0]);
        sigaction(SIGINT, &old_interrupt, NULL);
        return 127;
    }
    for (;;) {
        ssize_t i;
        ssize_t written = 0;
        got = read(ends[0], buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        while (written < got) {
            ssize_t n = write(STDOUT_FILENO, buffer + written, (size_t)(got - written));
            if (n <= 0) {
                break;
            }
            written += n;
        }
        for (i = 0; i < got; i++) {
            if (buffer[i] != '\r' && buffer[i] != '\0' && used + 1 < size) {
                output[used++] = buffer[i];
            }
        }
    }
    output[used] = '\0';
    close(ends[0]);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    sigaction(SIGINT, &old_interrupt, NULL);
    return exit_status(status);
}

/* The history is a hidden file beside the list: .jobcheck.history, and
   runs/.a.list.history for runs/a.list. */
static void history_path(char *out, size_t size)
{
    const char *slash = strrchr(list_path, '/');
    const char *base = slash ? slash + 1 : list_path;
    snprintf(out, size, "%.*s%s%s.history", (int)(base - list_path), list_path,
             base[0] == '.' ? "" : ".", base);
}

#define HISTORY_RULE "------------------------------------------------------------"

/* Adds a job to the history: when, whether it went through, and every watched
   value it was submitted with. */
static void history_add(int status, const char *output)
{
    char path[WATCH_PATH_SIZE + 16];
    char when[32];
    time_t now = time(NULL);
    int name_width = 4;
    FILE *file;
    int i;

    history_path(path, sizeof(path));
    file = fopen(path, "a");
    if (!file) {
        fprintf(stderr, "jobcheck: could not open %s: %s\n", path, strerror(errno));
        return;
    }
    fseek(file, 0, SEEK_END);
    if (ftell(file) > 0) {
        fputs(HISTORY_RULE "\n", file);
    }
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    if (status == 0) {
        fprintf(file, "%s   submitted\n", when);
    } else {
        fprintf(file, "%s   FAILED (exit status %d)\n", when, status);
    }
    fprintf(file, "command: %s\n", list.command);

    for (i = 0; i < list.count; i++) {
        int width = (int)strlen(list.watches[i].name);
        if (width > name_width) {
            name_width = width;
        }
    }
    for (i = 0; i < list.count; i++) {
        const Watch *watch = &list.watches[i];
        if (i == 0 || strcmp(watch->path, list.watches[i - 1].path) != 0) {
            fprintf(file, "%s\n", watch->path);
        }
        fprintf(file, "    %-*s  %s", name_width, watch->name, items[i].value);
        if (watch->has_last && strcmp(watch->last, items[i].value) != 0) {
            fprintf(file, "   (was %s)", watch->last);
        }
        fputc('\n', file);
    }
    if (request_text[0]) {
        fprintf(file, "request: %s\n", request_text);
    }
    if (output[0]) {
        bool line_start = true;
        fputs("output:\n", file);
        for (; *output; output++) {
            if (line_start) {
                fputs("    ", file);
            }
            fputc(*output, file);
            line_start = *output == '\n';
        }
        if (!line_start) {
            fputc('\n', file);
        }
    }
    fclose(file);
}

/* Puts back what the files had before the edited values were written into
   them; the values are edits again after it. Returns how many could not be
   put back. */
static int restore_written(void)
{
    int failed = 0;
    int i;
    for (i = 0; i < list.count; i++) {
        Item *item = &items[i];
        if (!item->written) {
            continue;
        }
        failed += !watch_file_set_value(list.watches[i].path, list.watches[i].line, item->old);
        memcpy(item->value, item->old, sizeof(item->value));
        item->written = false;
        item->has_edit = true;
    }
    return failed;
}

/* Returns when something stands in the way; a submission does not return. */
static void submit(void)
{
    static char output[65536];
    int changed;
    int open = 0;
    int missing = 0;
    int written = 0;
    int status;
    int i;

    if (list.command[0] == '\0') {
        snprintf(message, sizeof(message), "No submit command yet: press w, then c.");
        return;
    }
    /* A file may have been edited since the values were put on the screen. */
    changed = resolve();
    for (i = 0; i < list.count; i++) {
        missing += !items[i].found;
        open += items[i].found && !items[i].ticked;
    }
    if (changed > 0) {
        snprintf(message, sizeof(message),
                 "%d value%s changed in the files just now and lost the tick. Check again.",
                 changed, changed == 1 ? "" : "s");
        return;
    }
    if (missing > 0) {
        snprintf(message, sizeof(message),
                 "%d watched line%s no longer in the file. Press w to fix the list.", missing,
                 missing == 1 ? " is" : "s are");
        return;
    }
    if (open > 0) {
        snprintf(message, sizeof(message), "Not yet: %d value%s still to tick.", open,
                 open == 1 ? "" : "s");
        return;
    }

    /* Now, and not before, the values typed here go into the files. */
    for (i = 0; i < list.count; i++) {
        Item *item = &items[i];
        if (!item->has_edit) {
            continue;
        }
        if (!watch_file_set_value(list.watches[i].path, list.watches[i].line, item->edited)) {
            restore_written();
            resolve();
            snprintf(message, sizeof(message), "Could not write %.60s to %.120s. Not submitted.",
                     list.watches[i].name, list.watches[i].path);
            return;
        }
        memcpy(item->old, item->value, sizeof(item->old));
        memcpy(item->value, item->edited, sizeof(item->value));
        item->has_edit = false;
        item->written = true;
        written++;
    }
    if (resolve() > 0) {
        restore_written();
        resolve();
        snprintf(message, sizeof(message),
                 "A file did not read back as it was written. Not submitted; check again.");
        return;
    }

    terminal_restore();
    printf("jobcheck: %d value%s checked, running: %s\n", list.count, list.count == 1 ? "" : "s",
           list.command);
    fflush(stdout);
    status = run_command(list.command, output, sizeof(output));
    history_add(status, output);

    if (status == 0) {
        for (i = 0; i < list.count; i++) {
            memcpy(list.watches[i].last, items[i].value, sizeof(list.watches[i].last));
            list.watches[i].has_last = true;
        }
    } else {
        int failed = restore_written();
        fprintf(stderr, "jobcheck: the command ended with status %d; nothing was submitted.\n",
                status);
        if (written > 0 && failed == 0) {
            fprintf(stderr, "jobcheck: the %d edited value%s taken out of the files again.\n",
                    written, written == 1 ? " was" : "s were");
        } else if (failed > 0) {
            fprintf(stderr, "jobcheck: %d edited value%s could not be taken out of the files.\n",
                    failed, failed == 1 ? "" : "s");
        }
    }
    if (!watch_list_save(&list, list_path)) {
        fprintf(stderr, "jobcheck: could not write %s: %s\n", list_path, strerror(errno));
    }
    exit(status);
}

/* Shows the history, the last job at the bottom of the screen. */
static void view_history(void)
{
    char path[WATCH_PATH_SIZE + 16];
    WatchFile file;
    int top;

    history_path(path, sizeof(path));
    if (!watch_file_load(&file, path) || file.count == 0) {
        watch_file_free(&file);
        snprintf(message, sizeof(message), "Nothing has been submitted from this list yet.");
        return;
    }
    top = file.count;
    for (;;) {
        int height;
        int last_top;
        int key;
        int i;

        frame_begin();
        height = rows - 3 > 1 ? rows - 3 : 1;
        last_top = file.count > height ? file.count - height : 0;
        if (top > last_top) {
            top = last_top;
        }

        fputs(BOLD " Submissions" PLAIN "   ", stdout);
        put_text(path, columns - 16);
        line_end();
        line_end();
        for (i = top; i < top + height; i++) {
            if (i < file.count) {
                const char *line = file.lines[i];
                if (strcmp(line, HISTORY_RULE) == 0) {
                    fputs(DIM, stdout);
                } else if (line[0] >= '0' && line[0] <= '9') {
                    printf(BOLD "%s", strstr(line, "FAILED") ? red : green);
                }
                putchar(' ');
                put_text_tabs(line, columns - 2, 4);
            }
            line_end();
        }
        fputs(DIM, stdout);
        printf(" lines %d to %d of %d   arrows and page keys scroll   esc back", top + 1,
               top + height < file.count ? top + height : file.count, file.count);
        line_end();
        frame_end();

        key = read_key();
        if (key == KEY_ESCAPE || key == 'q' || key == 'h' || key == KEY_LEFT ||
            key == KEY_CTRL_C) {
            break;
        }
        move_cursor(key, &top, last_top + 1, height);
    }
    watch_file_free(&file);
}

/* Runs the dry-run command outside the screen and comes back. */
static void dry_run(void)
{
    int edits = 0;
    int status;
    int c;
    int i;

    if (list.test[0] == '\0') {
        snprintf(message, sizeof(message), "No dry-run command yet: press w, then t.");
        return;
    }
    for (i = 0; i < list.count; i++) {
        edits += items[i].has_edit;
    }
    terminal_restore();
    printf("jobcheck: dry run: %s\n", list.test);
    if (edits > 0) {
        printf("jobcheck: %d edited value%s not in the files before the submission; the dry run "
               "sees the old one%s.\n",
               edits, edits == 1 ? " is" : "s are", edits == 1 ? "" : "s");
    }
    fflush(stdout);
    status = exit_status(system(list.test));
    printf("\njobcheck: the dry run ended with status %d. Press Enter to go back. ", status);
    fflush(stdout);
    while ((c = getchar()) != '\n' && c != EOF) {
    }
    clearerr(stdin);
    terminal_take();
    snprintf(message, sizeof(message), "The dry run ended with status %d.", status);
}

/* ---- the list of watched values ---- */

/* The list as rows of the screen: a row >= 0 is that watch, a row < 0 the
   heading of the file of watch -row - 1. */
static int layout(int *row_of, int cursor, int *cursor_row)
{
    int count = 0;
    int i;
    *cursor_row = 0;
    for (i = 0; i < list.count; i++) {
        if (i == 0 || strcmp(list.watches[i].path, list.watches[i - 1].path) != 0) {
            row_of[count++] = -i - 1;
        }
        if (i == cursor) {
            *cursor_row = count;
        }
        row_of[count++] = i;
    }
    return count;
}

static bool differs_from_last(int index)
{
    return items[index].found && list.watches[index].has_last &&
           strcmp(list.watches[index].last, effective(index)) != 0;
}

static void draw_list(bool editing, int cursor, int *top)
{
    static int row_of[WATCH_MAX * 2];
    int cursor_row;
    int row_count = layout(row_of, cursor, &cursor_row);
    int name_width = 4;
    int ticked = 0;
    int different = 0;
    int edits = 0;
    bool third_row = editing || request_text[0]; /* the dry run, or what the scripts ask for */
    int height;
    int i;

    for (i = 0; i < list.count; i++) {
        int width = text_columns(list.watches[i].name);
        if (width > name_width) {
            name_width = width;
        }
        ticked += items[i].ticked;
        different += differs_from_last(i);
        edits += items[i].has_edit;
    }
    if (name_width > 32) {
        name_width = 32;
    }

    frame_begin();
    height = rows - (third_row ? 6 : 5);
    if (height < 1) {
        height = 1;
    }
    if (cursor_row > 0 && cursor_row - 1 < *top) {
        *top = cursor_row - 1; /* with the heading of its file */
    }
    scroll_to(top, cursor_row, height);

    fputs(BOLD " jobcheck" PLAIN "   ", stdout);
    fputs(editing ? "editing the list of watched values" : "tick every value to submit", stdout);
    line_end();
    fputs(DIM " submit    " PLAIN, stdout);
    if (list.command[0]) {
        put_text(list.command, columns - 12);
    } else {
        printf("%s(no command set)", red);
    }
    line_end();
    if (editing) {
        fputs(DIM " dry run   " PLAIN, stdout);
        put_text(list.test[0] ? list.test : "(no command set)", columns - 12);
        line_end();
    } else if (third_row) {
        fputs(DIM " request   " PLAIN, stdout);
        put_text(request_text, columns - 12);
        line_end();
    }
    line_end();

    for (i = *top; i < *top + height; i++) {
        if (i < row_count && row_of[i] < 0) {
            fputs(BOLD " ", stdout);
            put_text(list.watches[-row_of[i] - 1].path, columns - 2);
        } else if (i < row_count) {
            int index = row_of[i];
            const Item *item = &items[index];
            const Watch *watch = &list.watches[index];
            const char *selected = index == cursor ? REVERSE : "";
            int used = 9 + name_width;
            int n;

            fputs(selected, stdout);
            if (editing) {
                fputs("       ", stdout);
            } else if (!item->found) {
                printf("   %s[!]" PLAIN "%s ", red, selected);
            } else if (item->ticked) {
                printf("   %s[x]" PLAIN "%s ", green, selected);
            } else {
                fputs("   [ ] ", stdout);
            }
            n = put_text(watch->name, name_width);
            put_spaces(name_width - n + 2);
            if (!item->found) {
                printf("%s", red);
                used += put_text("not found in the file", columns - used - 1);
            } else if (effective(index)[0] == '\0') {
                used += put_text("(no value)", columns - used - 1);
            } else {
                fputs(BOLD, stdout);
                used += put_text(effective(index), columns - used - 1);
            }
            if (item->found && (item->has_edit || differs_from_last(index))) {
                printf(PLAIN "%s%s", selected, yellow);
                if (item->has_edit) {
                    used += put_text("   edited, file has ", columns - used - 1);
                    used += put_text(item->value, columns - used - 1);
                }
                /* Not once more when it is what the file has. */
                if (differs_from_last(index) &&
                    !(item->has_edit && strcmp(watch->last, item->value) == 0)) {
                    used += put_text("   was ", columns - used - 1);
                    used += put_text(watch->last, columns - used - 1);
                }
            }
            if (item->found && columns - used > 14) {
                char where[32];
                snprintf(where, sizeof(where), "  line %d", watch->line);
                printf(PLAIN "%s" DIM "%s", selected, where);
                used += (int)strlen(where);
            }
            if (index == cursor) {
                fputs(PLAIN REVERSE, stdout); /* the bar to the right edge */
                put_spaces(columns - used);
            }
        } else if (list.count == 0 && i == *top) {
            fputs(editing ? "   Nothing is watched yet. Press a to pick a file and its lines."
                          : "   Nothing is watched yet. Press w to edit the list.",
                  stdout);
        }
        line_end();
    }

    putchar(' ');
    if (message[0]) {
        put_text(message, columns - 2);
    } else if (!editing && list.count > 0) {
        char status[200];
        int length;
        if (ticked == list.count) {
            length = snprintf(status, sizeof(status), "All %d checked. Enter submits.", list.count);
            printf("%s%s" PLAIN, green, status);
        } else {
            length = snprintf(status, sizeof(status), "%d of %d checked", ticked, list.count);
            fputs(status, stdout);
        }
        status[0] = '\0';
        if (different > 0) {
            snprintf(status, sizeof(status), "   %d not as last submitted", different);
        }
        if (edits > 0) {
            snprintf(status + strlen(status), sizeof(status) - strlen(status),
                     "   %d edited, written on submit", edits);
        }
        printf("%s", yellow);
        put_text(status, columns - 2 - length);
    }
    line_end();
    fputs(DIM, stdout);
    put_text(editing ? " a add  d remove  c submit command  t dry-run command  enter done  q quit"
                     : " space tick  e edit  enter submit  t test  r reread  h history  w list  q quit",
             columns - 1);
    line_end();
    frame_end();
}

static void edit_value(int index)
{
    Item *item = &items[index];
    char label[80];
    char text[WATCH_VALUE_SIZE];
    size_t length;

    if (!item->found) {
        snprintf(message, sizeof(message),
                 "This line is no longer in the file. Press w to fix the list.");
        return;
    }
    if (item->value[0] == '\0') {
        snprintf(message, sizeof(message), "This line has no value to type over.");
        return;
    }
    snprintf(label, sizeof(label), " %.60s =", list.watches[index].name);
    snprintf(text, sizeof(text), "%s", effective(index));
    if (!prompt(label, text, sizeof(text))) {
        return;
    }
    length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
    memmove(text, text + strspn(text, " \t"), strlen(text + strspn(text, " \t")) + 1);
    if (text[0] == '\0') {
        snprintf(message, sizeof(message), "A value cannot be emptied here.");
    } else if (strcmp(text, effective(index)) != 0) {
        item->has_edit = strcmp(text, item->value) != 0;
        memcpy(item->edited, text, sizeof(item->edited));
        item->ticked = false;
        update_request();
    }
}

/* Returns when the user quits; a submission does not return. */
static void run(bool editing)
{
    int cursor = 0;
    int top = 0;

    resolve();
    for (;;) {
        int key;

        if (cursor >= list.count) {
            cursor = list.count > 0 ? list.count - 1 : 0;
        }
        draw_list(editing, cursor, &top);
        key = read_key();
        message[0] = '\0';

        if (move_cursor(key, &cursor, list.count, rows - 7 > 1 ? rows - 7 : 1)) {
            continue;
        }
        if (key == 'q' || key == KEY_CTRL_C) {
            return;
        }
        if (editing) {
            switch (key) {
            case 'a':
                browse();
                resolve();
                break;
            case 'd':
                if (list.count > 0) {
                    remove_watch(cursor);
                    save();
                    resolve();
                }
                break;
            case 'c':
                if (prompt(" submit command:", list.command, sizeof(list.command))) {
                    save();
                }
                break;
            case 't':
                if (list.test[0] == '\0' && strncmp(list.command, "sbatch ", 7) == 0) {
                    snprintf(list.test, sizeof(list.test), "sbatch --test-only %.990s",
                             list.command + 7);
                    if (!prompt(" dry-run command:", list.test, sizeof(list.test))) {
                        list.test[0] = '\0';
                    }
                    save();
                } else if (prompt(" dry-run command:", list.test, sizeof(list.test))) {
                    save();
                }
                break;
            case 'w': case KEY_ENTER: case KEY_ESCAPE:
                editing = false;
                resolve();
                break;
            }
        } else {
            int changed;
            switch (key) {
            case ' ': case 'x':
                if (list.count == 0) {
                    break;
                }
                if (items[cursor].found) {
                    items[cursor].ticked = !items[cursor].ticked;
                } else {
                    snprintf(message, sizeof(message),
                             "This line is no longer in the file. Press w to fix the list.");
                }
                break;
            case 'e':
                if (list.count > 0) {
                    edit_value(cursor);
                }
                break;
            case KEY_ENTER:
                submit();
                break;
            case 't':
                dry_run();
                break;
            case 'h':
                view_history();
                break;
            case 'r':
                changed = resolve();
                snprintf(message, sizeof(message), "Read the files again: %d value%s changed.",
                         changed, changed == 1 ? "" : "s");
                break;
            case 'w':
                editing = true;
                break;
            }
        }
    }
}

static void usage(FILE *out)
{
    fputs("usage: jobcheck [edit] [-f LIST]\n"
          "\n"
          "Shows the values watched in the files of a job and runs the submit command\n"
          "once every one of them has been ticked.\n"
          "\n"
          "  edit      start in the list of watched values, to add or remove some\n"
          "  -f LIST   the file the list is kept in (default: .jobcheck)\n"
          "\n"
          "A value edited here is written to its file at the submission and not\n"
          "before, and taken out again if the command fails. Every job is added to\n"
          "the history, a hidden file beside the list (.jobcheck.history), with\n"
          "its values, the time, and whether it went through.\n"
          "\n"
          "Exits with the status of the submit command, or with 1 when nothing was\n"
          "submitted.\n",
          out);
}

int main(int argc, char **argv)
{
    bool editing = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "edit") == 0) {
            editing = true;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            list_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("jobcheck: needs a terminal; nothing was submitted.\n", stderr);
        return 2;
    }
    if (!getenv("NO_COLOR")) {
        green = "\x1b[32m";
        red = "\x1b[31m";
        yellow = "\x1b[33m";
    }

    if (!watch_list_load(&list, list_path) || list.count == 0) {
        editing = true;
    }
    terminal_take();
    run(editing);
    terminal_restore();
    return 1;
}
