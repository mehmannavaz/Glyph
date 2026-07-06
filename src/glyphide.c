/* glyphide.c — A graphical+text IDE for the Glyph programming language.
 *
 * Built with X11 (the Unix way — use what exists).
 *
 * Features:
 *   - Code editor with syntax highlighting (shapes colored differently)
 *   - Shape preview panel showing the block structure visually
 *   - Run button (executes the current code via the glyph interpreter)
 *   - Error/warning display
 *   - File open/save
 *   - Line numbers
 *
 * The IDE itself follows the language's philosophy: the code is text,
 * but the shapes [squire] (loop) <guard> {trigger} are highlighted
 * with distinct colors so you can SEE the program structure.
 *
 * Build: cc -o glyphide glyphide.c -lX11 -lm
 * Run:   ./glyphide [file.glyph]
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define MAX_LINES 10000
#define MAX_LINE_LEN 1024
#define EDITOR_X 0
#define EDITOR_Y 30
#define EDITOR_W 600
#define EDITOR_H 570
#define PREVIEW_X 600
#define PREVIEW_Y 30
#define PREVIEW_W 400
#define PREVIEW_H 570
#define CHAR_W 9
#define CHAR_H 16
#define LINES_VISIBLE (EDITOR_H / CHAR_H)

/* Colors */
#define C_BG         0x1e1e2e  /* dark background */
#define C_TEXT       0xcdd6f4  /* light text */
#define C_SQUIRE     0xf9e2af  /* yellow — [squire] */
#define C_LOOP       0xa6e3a1  /* green — (loop) */
#define C_GUARD      0xfab387  /* orange — <guard> */
#define C_TRIGGER    0xf38ba8  /* red — {trigger} */
#define C_COMMENT    0x6c7086  /* gray */
#define C_STRING     0xa6e3a1  /* green */
#define C_NUMBER     0xfab387  /* orange */
#define C_KEYWORD    0xcba6f7  /* purple */
#define C_LINENUM    0x585b70  /* dark gray */
#define C_CURSOR     0xf5e0dc  /* cursor color */
#define C_STATUS     0x313244  /* status bar */
#define C_BUTTON     0x45475a  /* button */
#define C_BUTTON_HOT 0x585b70  /* button hover */
#define C_ERROR      0xf38ba8  /* error text */

static Display *display;
static int screen;
static Window window;
static GC gc;
static GC gc_clear;
static unsigned long pixels[256];
static int has_color = 0;
static Font font;
static XFontStruct *font_struct;

/* Editor state */
static char lines[MAX_LINES][MAX_LINE_LEN];
static int num_lines = 0;
static int cursor_line = 0;
static int cursor_col = 0;
static int scroll_top = 0;
static char filename[512] = "";
static int modified = 0;
static char status_msg[256] = "Glyph IDE — press Ctrl+S to save, Ctrl+R to run, Ctrl+Q to quit";

/* Error state */
static char error_msg[1024] = "";
static int has_error = 0;

/* Run output */
static char run_output[8192] = "";
static int show_output = 0;

/* Button state */
static int mouse_x = 0, mouse_y = 0;
static int button_hover = -1;

/* Buttons: [x, y, w, h, label, action] */
enum { BTN_RUN=0, BTN_SAVE=1, BTN_OPEN=2, BTN_LINT=3, BTN_CLEAR=4, BTN_COUNT=5 };
static struct { int x, y, w, h; const char *label; } buttons[BTN_COUNT] = {
    {  10, 4, 80, 22, "Run (Ctrl+R)" },
    {  95, 4, 80, 22, "Save (Ctrl+S)" },
    { 180, 4, 80, 22, "Open (Ctrl+O)" },
    { 265, 4, 80, 22, "Lint" },
    { 350, 4, 80, 22, "Clear" },
};

unsigned long get_color(int rgb) {
    XColor xc;
    xc.red = ((rgb >> 16) & 0xFF) * 257;
    xc.green = ((rgb >> 8) & 0xFF) * 257;
    xc.blue = (rgb & 0xFF) * 257;
    xc.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(display, DefaultColormap(display, screen), &xc))
        return xc.pixel;
    return WhitePixel(display, screen);
}

void init_colors(void) {
    pixels[0] = get_color(C_BG);
    pixels[1] = get_color(C_TEXT);
    pixels[2] = get_color(C_SQUIRE);
    pixels[3] = get_color(C_LOOP);
    pixels[4] = get_color(C_GUARD);
    pixels[5] = get_color(C_TRIGGER);
    pixels[6] = get_color(C_COMMENT);
    pixels[7] = get_color(C_STRING);
    pixels[8] = get_color(C_NUMBER);
    pixels[9] = get_color(C_KEYWORD);
    pixels[10] = get_color(C_LINENUM);
    pixels[11] = get_color(C_CURSOR);
    pixels[12] = get_color(C_STATUS);
    pixels[13] = get_color(C_BUTTON);
    pixels[14] = get_color(C_BUTTON_HOT);
    pixels[15] = get_color(C_ERROR);
    has_color = 1;
}

void set_color(int idx) {
    XSetForeground(display, gc, pixels[idx]);
}

/* ------------------------------------------------------------------ */
/* File I/O                                                           */
/* ------------------------------------------------------------------ */

void load_file(const char *fname) {
    FILE *f = fopen(fname, "r");
    if (!f) {
        snprintf(status_msg, sizeof(status_msg), "Cannot open: %s", fname);
        return;
    }
    num_lines = 0;
    while (fgets(lines[num_lines], MAX_LINE_LEN, f)) {
        /* strip newline */
        int len = strlen(lines[num_lines]);
        if (len > 0 && lines[num_lines][len-1] == '\n')
            lines[num_lines][len-1] = '\0';
        num_lines++;
        if (num_lines >= MAX_LINES) break;
    }
    fclose(f);
    strncpy(filename, fname, sizeof(filename)-1);
    modified = 0;
    cursor_line = 0;
    cursor_col = 0;
    scroll_top = 0;
    snprintf(status_msg, sizeof(status_msg), "Loaded: %s (%d lines)", fname, num_lines);
}

void save_file(void) {
    if (filename[0] == '\0') {
        snprintf(status_msg, sizeof(status_msg), "No filename set. Use: glyphide file.glyph");
        return;
    }
    FILE *f = fopen(filename, "w");
    if (!f) {
        snprintf(status_msg, sizeof(status_msg), "Cannot save: %s", filename);
        return;
    }
    for (int i = 0; i < num_lines; i++) {
        fputs(lines[i], f);
        fputc('\n', f);
    }
    fclose(f);
    modified = 0;
    snprintf(status_msg, sizeof(status_msg), "Saved: %s", filename);
}

void new_file(void) {
    num_lines = 1;
    strcpy(lines[0], "# New Glyph program");
    strcpy(lines[1], "");
    strcpy(lines[2], "(main)");
    strcpy(lines[3], "  print \"Hello, Glyph!\"");
    num_lines = 4;
    filename[0] = '\0';
    modified = 0;
    cursor_line = 0;
    cursor_col = 0;
    scroll_top = 0;
    strcpy(status_msg, "New file");
}

/* ------------------------------------------------------------------ */
/* Run / Lint                                                         */
/* ------------------------------------------------------------------ */

void run_code(void) {
    /* Save to a temp file, run glyph, capture output */
    const char *tmpfile = "/tmp/glyphide_tmp.glyph";
    FILE *f = fopen(tmpfile, "w");
    if (!f) {
        snprintf(status_msg, sizeof(status_msg), "Cannot write temp file");
        return;
    }
    for (int i = 0; i < num_lines; i++) {
        fputs(lines[i], f);
        fputc('\n', f);
    }
    fclose(f);

    /* Run glyph interpreter, capture stdout+stderr */
    char cmd[512];
    char glyph_path[512];
    /* Find glyph binary — check same directory as glyphide, then PATH */
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n > 0) {
        self[n] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            strcpy(slash+1, "glyph");
            strcpy(glyph_path, self);
        } else {
            strcpy(glyph_path, "glyph");
        }
    } else {
        strcpy(glyph_path, "glyph");
    }

    snprintf(cmd, sizeof(cmd), "%s %s 2>&1", glyph_path, tmpfile);
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        snprintf(status_msg, sizeof(status_msg), "Cannot run glyph");
        return;
    }
    run_output[0] = '\0';
    size_t outlen = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) {
        size_t blen = strlen(buf);
        if (outlen + blen < sizeof(run_output) - 1) {
            strcpy(run_output + outlen, buf);
            outlen += blen;
        }
    }
    int rc = pclose(pipe);
    show_output = 1;
    if (rc == 0) {
        snprintf(status_msg, sizeof(status_msg), "Run OK (exit 0)");
        has_error = 0;
        error_msg[0] = '\0';
    } else {
        snprintf(status_msg, sizeof(status_msg), "Run failed (exit %d)", WEXITSTATUS(rc));
        has_error = 1;
        strncpy(error_msg, run_output, sizeof(error_msg)-1);
    }
}

void lint_code(void) {
    const char *tmpfile = "/tmp/glyphide_tmp.glyph";
    FILE *f = fopen(tmpfile, "w");
    if (!f) return;
    for (int i = 0; i < num_lines; i++) {
        fputs(lines[i], f);
        fputc('\n', f);
    }
    fclose(f);

    char cmd[512];
    char glyph_path[512];
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n > 0) {
        self[n] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) { strcpy(slash+1, "glyph"); strcpy(glyph_path, self); }
        else strcpy(glyph_path, "glyph");
    } else strcpy(glyph_path, "glyph");

    snprintf(cmd, sizeof(cmd), "%s lint %s 2>&1", glyph_path, tmpfile);
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return;
    run_output[0] = '\0';
    size_t outlen = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) {
        size_t blen = strlen(buf);
        if (outlen + blen < sizeof(run_output) - 1) {
            strcpy(run_output + outlen, buf);
            outlen += blen;
        }
    }
    pclose(pipe);
    show_output = 1;
    snprintf(status_msg, sizeof(status_msg), "Lint complete");
}

/* ------------------------------------------------------------------ */
/* Syntax highlighting                                                */
/* ------------------------------------------------------------------ */

int get_line_color(const char *line) {
    /* Skip whitespace */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return 1; /* empty */
    if (*line == '#') return 6; /* comment */
    if (*line == '[') return 2; /* squire */
    if (*line == '(') return 3; /* loop */
    if (*line == '<') return 4; /* guard */
    if (*line == '{') return 5; /* trigger */
    return 1; /* default text */
}

/* Draw a single line with syntax highlighting */
void draw_line(int line_idx, int x, int y) {
    if (line_idx >= num_lines) return;
    char *line = lines[line_idx];

    /* Determine line type for coloring */
    int base_color = get_line_color(line);

    /* Draw line number */
    set_color(10);
    char numbuf[16];
    snprintf(numbuf, sizeof(numbuf), "%4d ", line_idx + 1);
    XDrawString(display, window, gc, x, y, numbuf, strlen(numbuf));

    /* Draw the line content with shape-aware coloring */
    int content_x = x + 50;
    int len = strlen(line);

    /* Check if this line starts a shape block */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '[' || *p == '(' || *p == '<' || *p == '{') {
        /* Shape block header — color the bracket and name */
        char bracket = *p;
        int color_idx;
        const char *shape_name;
        switch (bracket) {
            case '[': color_idx = 2; shape_name = "SQUIRE"; break;
            case '(': color_idx = 3; shape_name = "LOOP"; break;
            case '<': color_idx = 4; shape_name = "GUARD"; break;
            case '{': color_idx = 5; shape_name = "TRIGGER"; break;
            default:  color_idx = 1; shape_name = ""; break;
        }
        set_color(color_idx);
        XDrawString(display, window, gc, content_x, y, line, len);
    } else if (*p == '#') {
        /* Comment */
        set_color(6);
        XDrawString(display, window, gc, content_x, y, line, len);
    } else if (*p == '\0') {
        /* Empty line — draw nothing */
    } else {
        /* Regular line — draw with keyword highlighting */
        /* For simplicity, draw the whole line in text color.
         * A more sophisticated version would tokenize and color each token. */
        set_color(1);
        XDrawString(display, window, gc, content_x, y, line, len);
    }
}

/* Draw the shape preview panel — shows block structure as colored boxes */
void draw_preview(void) {
    int px = PREVIEW_X + 10;
    int py = PREVIEW_Y + 30;
    int box_h = 20;
    int indent_w = 20;

    set_color(1);
    XDrawString(display, window, gc, PREVIEW_X + 10, PREVIEW_Y + 20,
                "SHAPE PREVIEW", 13);

    int stack[50];
    int depth = 0;
    int y = py;

    for (int i = 0; i < num_lines && y < PREVIEW_Y + PREVIEW_H - 20; i++) {
        char *line = lines[i];
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '#') continue;
        if (*p == '\0') continue;

        if (*p == '[' || *p == '(' || *p == '<' || *p == '{') {
            char bracket = *p;
            int color_idx;
            switch (bracket) {
                case '[': color_idx = 2; break;
                case '(': color_idx = 3; break;
                case '<': color_idx = 4; break;
                case '{': color_idx = 5; break;
                default:  color_idx = 1; break;
            }
            /* Find the name */
            char name[64] = "";
            const char *name_start = p + 1;
            const char *name_end = name_start;
            while (*name_end && *name_end != ' ' && *name_end != ']' &&
                   *name_end != ')' && *name_end != '>' && *name_end != '}') name_end++;
            int nlen = name_end - name_start;
            if (nlen > 63) nlen = 63;
            memcpy(name, name_start, nlen);
            name[nlen] = '\0';

            /* Draw box */
            int bx = px + depth * indent_w;
            int bw = PREVIEW_W - 30 - depth * indent_w;
            set_color(color_idx);
            XFillRectangle(display, window, gc, bx, y, bw, box_h);
            set_color(0);
            char label[128];
            const char *stype;
            switch (bracket) {
                case '[': stype = "["; break;
                case '(': stype = "("; break;
                case '<': stype = "<"; break;
                case '{': stype = "{"; break;
                default:  stype = ""; break;
            }
            snprintf(label, sizeof(label), "%s %s", stype, name);
            XDrawString(display, window, gc, bx + 5, y + 14, label, strlen(label));

            if (depth < 49) stack[depth++] = bracket;
            y += box_h + 2;
        } else if (depth > 0) {
            /* Body line — draw a small marker */
            int bx = px + depth * indent_w;
            set_color(13);
            XFillRectangle(display, window, gc, bx, y, 5, box_h);
            y += box_h + 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

void draw_editor(void) {
    /* Fill editor background */
    set_color(0);
    XFillRectangle(display, window, gc, EDITOR_X, EDITOR_Y, EDITOR_W, EDITOR_H);

    /* Draw lines */
    for (int i = 0; i < LINES_VISIBLE && scroll_top + i < num_lines; i++) {
        draw_line(scroll_top + i, EDITOR_X + 5, EDITOR_Y + 15 + i * CHAR_H);
    }

    /* Draw cursor */
    if (cursor_line >= scroll_top && cursor_line < scroll_top + LINES_VISIBLE) {
        int cy = EDITOR_Y + 15 + (cursor_line - scroll_top) * CHAR_H - 12;
        int cx = EDITOR_X + 50 + cursor_col * CHAR_W;
        set_color(11);
        XFillRectangle(display, window, gc, cx, cy, 2, CHAR_H);
    }
}

void draw_buttons(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        int color = (button_hover == i) ? 14 : 13;
        set_color(color);
        XFillRectangle(display, window, gc, buttons[i].x, buttons[i].y,
                       buttons[i].w, buttons[i].h);
        set_color(1);
        XDrawString(display, window, gc, buttons[i].x + 5, buttons[i].y + 15,
                    buttons[i].label, strlen(buttons[i].label));
    }
}

void draw_status_bar(void) {
    set_color(12);
    XFillRectangle(display, window, gc, 0, 600, 1000, 24);
    set_color(1);
    XDrawString(display, window, gc, 10, 616, status_msg, strlen(status_msg));

    /* Show cursor position */
    char pos[64];
    snprintf(pos, sizeof(pos), "L%d:C%d %s", cursor_line+1, cursor_col+1,
             modified ? "[modified]" : "");
    set_color(1);
    XDrawString(display, window, gc, 900, 616, pos, strlen(pos));
}

void draw_output_panel(void) {
    /* If showing output, overlay it on the preview panel */
    if (!show_output) return;
    set_color(0);
    XFillRectangle(display, window, gc, PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H);
    set_color(1);
    XDrawString(display, window, gc, PREVIEW_X + 10, PREVIEW_Y + 20,
                "OUTPUT (press Esc to close)", 27);
    int y = PREVIEW_Y + 40;
    char *p = run_output;
    while (*p && y < PREVIEW_Y + PREVIEW_H - 10) {
        char *nl = strchr(p, '\n');
        int len = nl ? (nl - p) : strlen(p);
        if (len > 0) {
            set_color(has_error ? 15 : 1);
            XDrawString(display, window, gc, PREVIEW_X + 10, y, p, len);
        }
        y += CHAR_H;
        if (!nl) break;
        p = nl + 1;
    }
}

void redraw(void) {
    /* Clear window */
    set_color(0);
    XFillRectangle(display, window, gc, 0, 0, 1000, 624);

    draw_buttons();
    draw_editor();
    draw_preview();
    if (show_output) draw_output_panel();
    draw_status_bar();
}

/* ------------------------------------------------------------------ */
/* Event handling                                                     */
/* ------------------------------------------------------------------ */

void handle_keypress(XKeyEvent *ev) {
    char buf[32];
    KeySym keysym;
    int len = XLookupString(ev, buf, sizeof(buf), &keysym, NULL);

    if (ev->state & ControlMask) {
        /* Ctrl+key */
        switch (keysym) {
            case 'r': case 'R': run_code(); break;
            case 's': case 'S': save_file(); break;
            case 'o': case 'O': {
                /* Simple: prompt in status bar (v1: just load /tmp/glyphide_tmp.glyph) */
                load_file("/tmp/glyphide_tmp.glyph");
                break;
            }
            case 'q': case 'Q': exit(0); break;
        }
        return;
    }

    switch (keysym) {
        case XK_Escape: show_output = 0; break;
        case XK_Return: {
            /* Insert new line, split current line at cursor */
            if (num_lines < MAX_LINES - 1) {
                /* Shift lines down */
                for (int i = num_lines; i > cursor_line + 1; i--)
                    strcpy(lines[i], lines[i-1]);
                /* Split: move text after cursor to new line */
                char *cur = lines[cursor_line];
                int cur_len = strlen(cur);
                if (cursor_col < cur_len) {
                    strcpy(lines[cursor_line + 1], cur + cursor_col);
                    cur[cursor_col] = '\0';
                } else {
                    lines[cursor_line + 1][0] = '\0';
                }
                /* Auto-indent: copy whitespace from current line */
                int indent = 0;
                while (indent < cursor_col && (cur[indent] == ' ' || cur[indent] == '\t'))
                    indent++;
                /* Add 2 extra spaces if current line starts a shape block */
                const char *p = cur;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '[' || *p == '(' || *p == '<' || *p == '{')
                    indent += 2;
                /* Apply indent to new line */
                char new_line[MAX_LINE_LEN];
                memset(new_line, ' ', indent);
                new_line[indent] = '\0';
                strcat(new_line, lines[cursor_line + 1]);
                strcpy(lines[cursor_line + 1], new_line);

                num_lines++;
                cursor_line++;
                cursor_col = indent;
                modified = 1;
            }
            break;
        }
        case XK_BackSpace: {
            if (cursor_col > 0) {
                char *cur = lines[cursor_line];
                memmove(cur + cursor_col - 1, cur + cursor_col, strlen(cur) - cursor_col + 1);
                cursor_col--;
                modified = 1;
            } else if (cursor_line > 0) {
                /* Merge with previous line */
                int prev_len = strlen(lines[cursor_line - 1]);
                int cur_len = strlen(lines[cursor_line]);
                if (prev_len + cur_len < MAX_LINE_LEN) {
                    strcat(lines[cursor_line - 1], lines[cursor_line]);
                    cursor_col = prev_len;
                    for (int i = cursor_line; i < num_lines - 1; i++)
                        strcpy(lines[i], lines[i+1]);
                    num_lines--;
                    cursor_line--;
                    modified = 1;
                }
            }
            break;
        }
        case XK_Delete: {
            char *cur = lines[cursor_line];
            int cur_len = strlen(cur);
            if (cursor_col < cur_len) {
                memmove(cur + cursor_col, cur + cursor_col + 1, cur_len - cursor_col);
                modified = 1;
            } else if (cursor_line < num_lines - 1) {
                int next_len = strlen(lines[cursor_line + 1]);
                if (cur_len + next_len < MAX_LINE_LEN) {
                    strcat(cur, lines[cursor_line + 1]);
                    for (int i = cursor_line + 1; i < num_lines - 1; i++)
                        strcpy(lines[i], lines[i+1]);
                    num_lines--;
                    modified = 1;
                }
            }
            break;
        }
        case XK_Left:  if (cursor_col > 0) cursor_col--; break;
        case XK_Right: if (cursor_col < (int)strlen(lines[cursor_line])) cursor_col++; break;
        case XK_Up:    if (cursor_line > 0) cursor_line--; break;
        case XK_Down:  if (cursor_line < num_lines - 1) cursor_line++; break;
        case XK_Home:  cursor_col = 0; break;
        case XK_End:   cursor_col = strlen(lines[cursor_line]); break;
        case XK_Page_Up:   cursor_line -= LINES_VISIBLE; if (cursor_line < 0) cursor_line = 0; break;
        case XK_Page_Down: cursor_line += LINES_VISIBLE; if (cursor_line >= num_lines) cursor_line = num_lines - 1; break;
        default:
            if (len == 1 && buf[0] >= 32 && buf[0] < 127) {
                char *cur = lines[cursor_line];
                int cur_len = strlen(cur);
                if (cur_len < MAX_LINE_LEN - 1) {
                    memmove(cur + cursor_col + 1, cur + cursor_col, cur_len - cursor_col + 1);
                    cur[cursor_col] = buf[0];
                    cursor_col++;
                    modified = 1;
                }
            }
            break;
    }

    /* Adjust scroll */
    if (cursor_line < scroll_top) scroll_top = cursor_line;
    if (cursor_line >= scroll_top + LINES_VISIBLE)
        scroll_top = cursor_line - LINES_VISIBLE + 1;
}

void handle_buttonpress(XButtonEvent *ev) {
    /* Check button clicks */
    for (int i = 0; i < BTN_COUNT; i++) {
        if (ev->x >= buttons[i].x && ev->x < buttons[i].x + buttons[i].w &&
            ev->y >= buttons[i].y && ev->y < buttons[i].y + buttons[i].h) {
            switch (i) {
                case BTN_RUN:  run_code(); break;
                case BTN_SAVE: save_file(); break;
                case BTN_OPEN: load_file("/tmp/glyphide_tmp.glyph"); break;
                case BTN_LINT: lint_code(); break;
                case BTN_CLEAR: show_output = 0; has_error = 0; break;
            }
            return;
        }
    }

    /* Click in editor — move cursor */
    if (ev->x >= EDITOR_X + 50 && ev->x < EDITOR_X + EDITOR_W &&
        ev->y >= EDITOR_Y && ev->y < EDITOR_Y + EDITOR_H) {
        int line = scroll_top + (ev->y - EDITOR_Y - 15) / CHAR_H;
        int col = (ev->x - EDITOR_X - 50) / CHAR_W;
        if (line >= 0 && line < num_lines) {
            cursor_line = line;
            int line_len = strlen(lines[line]);
            if (col < 0) col = 0;
            if (col > line_len) col = line_len;
            cursor_col = col;
        }
    }

    /* Click in preview — close output if showing */
    if (ev->x >= PREVIEW_X && ev->x < PREVIEW_X + PREVIEW_W) {
        if (show_output) show_output = 0;
    }
}

void handle_motion(XMotionEvent *ev) {
    mouse_x = ev->x;
    mouse_y = ev->y;
    int old_hover = button_hover;
    button_hover = -1;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (ev->x >= buttons[i].x && ev->x < buttons[i].x + buttons[i].w &&
            ev->y >= buttons[i].y && ev->y < buttons[i].y + buttons[i].h) {
            button_hover = i;
            break;
        }
    }
    if (old_hover != button_hover) {
        /* Redraw buttons area only */
        set_color(0);
        XFillRectangle(display, window, gc, 0, 0, 1000, 30);
        draw_buttons();
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                          */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open X display\n");
        return 1;
    }
    screen = DefaultScreen(display);

    init_colors();

    window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                 0, 0, 1000, 624, 0,
                                 pixels[0], pixels[0]);
    XStoreName(display, window, "Glyph IDE — graphical+text programming");

    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);

    /* Load font */
    font_struct = XLoadQueryFont(display, "9x15");
    if (!font_struct) font_struct = XLoadQueryFont(display, "fixed");
    if (!font_struct) {
        fprintf(stderr, "Cannot load font\n");
        return 1;
    }

    gc = XCreateGC(display, window, 0, NULL);
    XSetFont(display, gc, font_struct->fid);
    XSetBackground(display, gc, pixels[0]);

    XMapWindow(display, window);
    XFlush(display);

    /* Load file or start new */
    if (argc > 1) {
        load_file(argv[1]);
    } else {
        new_file();
    }

    /* Main event loop */
    while (1) {
        XEvent ev;
        XNextEvent(display, &ev);
        switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) redraw();
                break;
            case KeyPress:
                handle_keypress(&ev.xkey);
                redraw();
                break;
            case ButtonPress:
                handle_buttonpress(&ev.xbutton);
                redraw();
                break;
            case MotionNotify:
                handle_motion(&ev.xmotion);
                break;
            case ConfigureNotify:
                redraw();
                break;
        }
        XFlush(display);
    }

    XCloseDisplay(display);
    return 0;
}
