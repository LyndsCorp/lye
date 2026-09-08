/*
 * lye - Lynds Editor
 * Editor de texto TUI en C11 inspirado en GNU nano.
 * Interfaz en español, con atajos y números de línea.
 * Con resaltado de sintaxis mediante archivos .nanorc.
 */

#define _GNU_SOURCE

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>

/* Definiciones de teclas de control */
#define KEY_CTRL(x) ((x) & 0x1F)
#define KEY_CTRL_SLASH 0x1F
#define KEY_CTRL_S     19

#define VERSION "1.0"
#define EDITION "lye - Lynds Editor\nPrimera versión de lye.\n¡Espero que te guste! :)"

typedef struct Editor {
    char **buffer;
    int num_lines;
    int cursor_x, cursor_y;   // cursor_x es índice de byte dentro de la línea
    int top_line;
    int left_col;
    int modified;
    char *filename;
    char message[256];
    int msg_timeout;
    char **clipboard;
    int clipboard_lines;
    // Historial con posición del cursor
    struct {
        char ***buffers;
        int *num_lines;
        int *cursor_x;
        int *cursor_y;
        int count;
        int index;
        int max;
    } history;
    char last_search[256];
    int readonly;
    int welcome_shown;
    mode_t original_mode;
    void *syntax_rule;
    int *syntax_state;       // estado actual de start/end para la línea actual (se usa al dibujar)
    int **line_states;       // caché de estados por línea (para cada regla start/end)
    int line_states_capacity;
} Editor;

#include "nanorc.h"

/* Prototipos */
void init_curses(void);
void cleanup(void);
int check_resize_ncurses(Editor *ed);
void draw_screen(Editor *ed);
void draw_status_bar(Editor *ed, const char *msg);
void show_help(Editor *ed);
void print_help_text(void);
void load_file(Editor *ed, const char *filename);
int save_file(Editor *ed, const char *filename);
void push_history(Editor *ed);
void undo(Editor *ed);
void redo(Editor *ed);
void free_buffer(Editor *ed);
void free_clipboard(Editor *ed);
void free_history(Editor *ed);
void insert_string(Editor *ed, const char *s);
void delete_char(Editor *ed);
void insert_newline(Editor *ed);
void delete_current_line(Editor *ed);
void cut_line(Editor *ed);
void paste_clipboard(Editor *ed);
void search(Editor *ed);
void search_next(Editor *ed);
void goto_line(Editor *ed);
void confirm_exit(Editor *ed);
void handle_input(Editor *ed, int ch);
int read_line_from_user(Editor *ed, const char *prompt, char *buffer, int maxlen);
int read_filename_with_default(Editor *ed, const char *prompt, char *buffer, int maxlen, const char *def);
char **duplicate_buffer(Editor *ed, int *num_lines_out);
char **duplicate_buffer_from_history(Editor *ed, int index, int *num_lines_out);
char *my_strdup(const char *s);
int read_file_line(FILE *fp, char **line, size_t *len);
void adjust_view(Editor *ed);
void draw_truncated_utf8(int y, int x, int max_bytes, const char *text);
char *sanitize_string(const char *str);
int is_valid_utf8(const unsigned char *s, size_t len);
int prev_char_pos(const char *s, int pos);
int next_char_pos(const char *s, int pos, int len);
int visual_width_until(const char *line, int pos);
void invalidate_syntax_cache(Editor *ed);

Editor *global_ed = NULL;

static const char *g_help_lines[] = {
    "Atajos de teclado principales:",
    "",
    "^G   Mostrar esta ayuda",
    "^S   Guardar archivo",
    "^O   Guardar como (pregunta nombre)",
    "^F   Buscar texto",
    "^W   Buscar siguiente (después de ^F)",
    "^K   Cortar línea actual",
    "^U   Pegar línea(s) del portapapeles",
    "^Z   Deshacer último cambio",
    "^Y   Rehacer cambio deshecho",
    "^/   Ir a línea específica",
    "^X   Salir del editor",
    "^L   Refrescar pantalla",
    "",
    "Movimiento: flechas, Inicio, Fin, RePág, AvPág",
    "Edición: Insertar, Supr, Retroceso, Enter",
    "",
    "Hay una línea vacía siempre disponible al final.",
    "",
    "Presione cualquier tecla para volver..."
};
static const int g_help_lines_count = sizeof(g_help_lines) / sizeof(g_help_lines[0]);

char *my_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

int is_valid_utf8(const unsigned char *s, size_t len) {
    if (len == 0) return 0;
    unsigned char c = s[0];
    if (c < 0x80) return len == 1;
    if ((c & 0xE0) == 0xC0) return len == 2 && (s[1] & 0xC0) == 0x80;
    if ((c & 0xF0) == 0xE0) return len == 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80;
    if ((c & 0xF8) == 0xF0) return len == 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80;
    return 0;
}

char *sanitize_string(const char *str) {
    if (!str) return my_strdup("");
    size_t len = strlen(str);
    char *safe = malloc(len + 1);
    if (!safe) return my_strdup("");
    size_t out = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)str[i];
        if (c < 0x20 || c == 0x7F) {
            safe[out++] = '?';
            i++;
            continue;
        }
        if (c < 0x80) {
            safe[out++] = c;
            i++;
            continue;
        }
        size_t seq_len;
        if ((c & 0xE0) == 0xC0) seq_len = 2;
        else if ((c & 0xF0) == 0xE0) seq_len = 3;
        else if ((c & 0xF8) == 0xF0) seq_len = 4;
        else { safe[out++] = '?'; i++; continue; }
        if (i + seq_len > len || !is_valid_utf8((const unsigned char*)str + i, seq_len)) {
            safe[out++] = '?';
            i++;
        } else {
            memcpy(safe + out, str + i, seq_len);
            out += seq_len;
            i += seq_len;
        }
    }
    safe[out] = '\0';
    return safe;
}

int read_file_line(FILE *fp, char **line, size_t *len) {
    size_t capacity = 128;
    size_t size = 0;
    char *buffer = malloc(capacity);
    if (!buffer) return -1;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') break;
        if (size + 1 >= capacity) {
            capacity *= 2;
            char *new_buffer = realloc(buffer, capacity);
            if (!new_buffer) {
                free(buffer);
                return -1;
            }
            buffer = new_buffer;
        }
        buffer[size++] = (char)c;
    }
    buffer[size] = '\0';
    if (c == EOF && size == 0) {
        free(buffer);
        return -1;
    }
    *line = buffer;
    *len = size;
    return (int)size;
}

void draw_truncated_utf8(int y, int x, int max_bytes, const char *text) {
    if (max_bytes <= 0) return;
    size_t len = strlen(text);
    if (len <= (size_t)max_bytes) {
        mvaddstr(y, x, text);
        return;
    }
    size_t end = max_bytes;
    while (end > 0 && (text[end] & 0xC0) == 0x80) end--;
    mvaddnstr(y, x, text, (int)end);
}

void init_curses(void) {
    initscr();
    raw();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(1);
    if (has_colors()) {
        start_color();
        use_default_colors();  // permite -1 en init_pair
        init_pair(1, COLOR_WHITE, COLOR_BLUE);
        init_pair(2, COLOR_GREEN, COLOR_BLACK);
        init_pair(3, COLOR_CYAN, COLOR_BLACK);
        init_pair(4, COLOR_BLACK, COLOR_WHITE);
        init_pair(5, COLOR_RED, COLOR_BLACK);
    }
}

void cleanup(void) {
    endwin();
}

void adjust_view(Editor *ed) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int edit_rows = rows - 3;
    int total_lines = ed->num_lines + 1;
    int line_num_width = 1;
    int temp = total_lines;
    while (temp >= 10) { temp /= 10; line_num_width++; }
    line_num_width++;
    int edit_cols = cols - line_num_width - 1;

    if (ed->cursor_y < ed->top_line) ed->top_line = ed->cursor_y;
    if (ed->cursor_y >= ed->top_line + edit_rows) ed->top_line = ed->cursor_y - edit_rows + 1;
    if (ed->top_line < 0) ed->top_line = 0;
    if (ed->top_line > total_lines - edit_rows) ed->top_line = total_lines - edit_rows;
    if (ed->top_line < 0) ed->top_line = 0;

    int block_width = edit_cols - 1;
    if (block_width <= 0) block_width = 1;

    if (ed->cursor_x < ed->left_col) {
        ed->left_col = ed->cursor_x;
    }
    int vis_width = visual_width_until(ed->buffer[ed->cursor_y], ed->cursor_x);
    if (vis_width >= ed->left_col + block_width) {
        int target = vis_width - block_width + 1;
        int byte_off = 0;
        const char *line = ed->buffer[ed->cursor_y];
        int w = 0;
        while (byte_off < ed->cursor_x) {
            int next = next_char_pos(line, byte_off, strlen(line));
            char tmp[8];
            int l = next - byte_off;
            memcpy(tmp, line + byte_off, l);
            tmp[l] = '\0';
            wchar_t wc;
            mbtowc(&wc, tmp, l);
            int cw = wcwidth(wc);
            if (w + cw > target) break;
            w += cw;
            byte_off = next;
        }
        ed->left_col = byte_off;
    }
    if (ed->left_col < 0) ed->left_col = 0;
}

int check_resize_ncurses(Editor *ed) {
    if (is_term_resized(0, 0)) {
        resize_term(0, 0);
        clearok(stdscr, TRUE);
        adjust_view(ed);
        draw_screen(ed);
        return 1;
    }
    return 0;
}

void free_buffer(Editor *ed) {
    if (ed->buffer) {
        for (int i = 0; i < ed->num_lines; i++) free(ed->buffer[i]);
        free(ed->buffer);
        ed->buffer = NULL;
    }
    ed->num_lines = 0;
    if (ed->syntax_state) {
        free(ed->syntax_state);
        ed->syntax_state = NULL;
        ed->syntax_rule = NULL;
    }
    invalidate_syntax_cache(ed);
}

void free_clipboard(Editor *ed) {
    if (ed->clipboard) {
        for (int i = 0; i < ed->clipboard_lines; i++) free(ed->clipboard[i]);
        free(ed->clipboard);
        ed->clipboard = NULL;
        ed->clipboard_lines = 0;
    }
}

void free_history(Editor *ed) {
    if (ed->history.buffers) {
        for (int i = 0; i < ed->history.count; i++) {
            for (int j = 0; j < ed->history.num_lines[i]; j++) free(ed->history.buffers[i][j]);
            free(ed->history.buffers[i]);
        }
        free(ed->history.buffers);
        free(ed->history.num_lines);
        free(ed->history.cursor_x);
        free(ed->history.cursor_y);
        ed->history.buffers = NULL;
        ed->history.num_lines = NULL;
        ed->history.cursor_x = NULL;
        ed->history.cursor_y = NULL;
        ed->history.count = 0;
        ed->history.index = -1;
    }
}

char **duplicate_buffer(Editor *ed, int *num_lines_out) {
    char **copy = malloc(ed->num_lines * sizeof(char *));
    if (!copy) return NULL;
    for (int i = 0; i < ed->num_lines; i++) {
        copy[i] = my_strdup(ed->buffer[i]);
        if (!copy[i]) {
            for (int j = 0; j < i; j++) free(copy[j]);
            free(copy);
            return NULL;
        }
    }
    *num_lines_out = ed->num_lines;
    return copy;
}

void push_history(Editor *ed) {
    if (ed->history.count >= ed->history.max) {
        // Liberar la entrada más antigua
        for (int j = 0; j < ed->history.num_lines[0]; j++) free(ed->history.buffers[0][j]);
        free(ed->history.buffers[0]);
        memmove(ed->history.buffers, ed->history.buffers + 1, (ed->history.count - 1) * sizeof(char **));
        memmove(ed->history.num_lines, ed->history.num_lines + 1, (ed->history.count - 1) * sizeof(int));
        memmove(ed->history.cursor_x, ed->history.cursor_x + 1, (ed->history.count - 1) * sizeof(int));
        memmove(ed->history.cursor_y, ed->history.cursor_y + 1, (ed->history.count - 1) * sizeof(int));
        ed->history.count--;
        if (ed->history.index > 0) ed->history.index--;
    }
    int num_lines_copy;
    char **copy = duplicate_buffer(ed, &num_lines_copy);
    if (!copy) return;

    char ***new_buffers = realloc(ed->history.buffers, (ed->history.count + 1) * sizeof(char **));
    if (!new_buffers) {
        for (int j = 0; j < num_lines_copy; j++) free(copy[j]);
        free(copy);
        return;
    }
    ed->history.buffers = new_buffers;

    int *new_num_lines = realloc(ed->history.num_lines, (ed->history.count + 1) * sizeof(int));
    if (!new_num_lines) {
        ed->history.buffers = new_buffers;
        return;
    }
    ed->history.num_lines = new_num_lines;

    int *new_cx = realloc(ed->history.cursor_x, (ed->history.count + 1) * sizeof(int));
    if (!new_cx) {
        ed->history.buffers = new_buffers;
        ed->history.num_lines = new_num_lines;
        return;
    }
    ed->history.cursor_x = new_cx;

    int *new_cy = realloc(ed->history.cursor_y, (ed->history.count + 1) * sizeof(int));
    if (!new_cy) {
        ed->history.buffers = new_buffers;
        ed->history.num_lines = new_num_lines;
        ed->history.cursor_x = new_cx;
        return;
    }
    ed->history.cursor_y = new_cy;

    ed->history.buffers[ed->history.count] = copy;
    ed->history.num_lines[ed->history.count] = num_lines_copy;
    ed->history.cursor_x[ed->history.count] = ed->cursor_x;
    ed->history.cursor_y[ed->history.count] = ed->cursor_y;
    ed->history.count++;
    ed->history.index = ed->history.count - 1;

    // Eliminar entradas futuras (redo)
    for (int i = ed->history.index + 1; i < ed->history.count; i++) {
        for (int j = 0; j < ed->history.num_lines[i]; j++) free(ed->history.buffers[i][j]);
        free(ed->history.buffers[i]);
    }
    ed->history.count = ed->history.index + 1;

    invalidate_syntax_cache(ed);
}

char **duplicate_buffer_from_history(Editor *ed, int index, int *num_lines_out) {
    if (index < 0 || index >= ed->history.count) return NULL;
    char **copy = malloc(ed->history.num_lines[index] * sizeof(char *));
    if (!copy) return NULL;
    for (int i = 0; i < ed->history.num_lines[index]; i++) {
        copy[i] = my_strdup(ed->history.buffers[index][i]);
        if (!copy[i]) {
            for (int j = 0; j < i; j++) free(copy[j]);
            free(copy);
            return NULL;
        }
    }
    *num_lines_out = ed->history.num_lines[index];
    return copy;
}

void undo(Editor *ed) {
    if (ed->history.index > 0) {
        ed->history.index--;
        int new_num_lines;
        char **new_buffer = duplicate_buffer_from_history(ed, ed->history.index, &new_num_lines);
        if (!new_buffer) {
            draw_status_bar(ed, "Error al deshacer");
            return;
        }
        free_buffer(ed);
        ed->buffer = new_buffer;
        ed->num_lines = new_num_lines;
        ed->cursor_x = ed->history.cursor_x[ed->history.index];
        ed->cursor_y = ed->history.cursor_y[ed->history.index];
        if (ed->cursor_y > ed->num_lines) ed->cursor_y = ed->num_lines;
        if (ed->num_lines > 0 && ed->cursor_x > (int)strlen(ed->buffer[ed->cursor_y < ed->num_lines ? ed->cursor_y : ed->num_lines-1])) {
            ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y < ed->num_lines ? ed->cursor_y : ed->num_lines-1]);
        }
        ed->left_col = 0;
        ed->modified = 1;
        invalidate_syntax_cache(ed);
        adjust_view(ed);
        draw_screen(ed);
    } else {
        draw_status_bar(ed, "Nada que deshacer");
    }
}

void redo(Editor *ed) {
    if (ed->history.index < ed->history.count - 1) {
        ed->history.index++;
        int new_num_lines;
        char **new_buffer = duplicate_buffer_from_history(ed, ed->history.index, &new_num_lines);
        if (!new_buffer) {
            draw_status_bar(ed, "Error al rehacer");
            return;
        }
        free_buffer(ed);
        ed->buffer = new_buffer;
        ed->num_lines = new_num_lines;
        ed->cursor_x = ed->history.cursor_x[ed->history.index];
        ed->cursor_y = ed->history.cursor_y[ed->history.index];
        if (ed->cursor_y > ed->num_lines) ed->cursor_y = ed->num_lines;
        if (ed->num_lines > 0 && ed->cursor_x > (int)strlen(ed->buffer[ed->cursor_y < ed->num_lines ? ed->cursor_y : ed->num_lines-1])) {
            ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y < ed->num_lines ? ed->cursor_y : ed->num_lines-1]);
        }
        ed->left_col = 0;
        ed->modified = 1;
        invalidate_syntax_cache(ed);
        adjust_view(ed);
        draw_screen(ed);
    } else {
        draw_status_bar(ed, "Nada que rehacer");
    }
}

void load_file(Editor *ed, const char *filename) {
    struct stat st;
    if (stat(filename, &st) != 0) {
        if (errno == ENOENT) {
            char msg[512];
            snprintf(msg, sizeof(msg), "El archivo «%s» no existe. Se creará al guardar.", filename);
            free_buffer(ed);
            ed->buffer = malloc(sizeof(char *));
            if (!ed->buffer) {
                draw_status_bar(ed, "Error de memoria");
                return;
            }
            ed->buffer[0] = my_strdup("");
            if (!ed->buffer[0]) {
                free(ed->buffer);
                ed->buffer = NULL;
                draw_status_bar(ed, "Error de memoria");
                return;
            }
            ed->num_lines = 1;
            free(ed->filename);
            ed->filename = my_strdup(filename);
            ed->modified = 0;
            ed->cursor_x = 0;
            ed->cursor_y = 0;
            ed->top_line = 0;
            ed->left_col = 0;
            ed->readonly = 0;
            ed->original_mode = 0644;
            ed->welcome_shown = 0;
            set_current_syntax(ed, ed->filename);
            reset_syntax_state(ed);
            invalidate_syntax_cache(ed);
            draw_status_bar(ed, msg);
            adjust_view(ed);
            return;
        } else {
            char error_msg[512];
            snprintf(error_msg, sizeof(error_msg), "Error leyendo «%s»: %s", filename, strerror(errno));
            free_buffer(ed);
            ed->buffer = malloc(sizeof(char *));
            if (!ed->buffer) {
                draw_status_bar(ed, "Error de memoria");
                return;
            }
            ed->buffer[0] = my_strdup("");
            if (!ed->buffer[0]) {
                free(ed->buffer);
                ed->buffer = NULL;
                draw_status_bar(ed, "Error de memoria");
                return;
            }
            ed->num_lines = 1;
            free(ed->filename);
            ed->filename = my_strdup(filename);
            ed->modified = 0;
            ed->cursor_x = 0;
            ed->cursor_y = 0;
            ed->top_line = 0;
            ed->left_col = 0;
            ed->readonly = 0;
            ed->original_mode = 0644;
            ed->welcome_shown = 0;
            set_current_syntax(ed, ed->filename);
            reset_syntax_state(ed);
            invalidate_syntax_cache(ed);
            draw_status_bar(ed, error_msg);
            adjust_view(ed);
            return;
        }
    }

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg), "Error leyendo «%s»: %s", filename, strerror(errno));
        free_buffer(ed);
        ed->buffer = malloc(sizeof(char *));
        if (!ed->buffer) {
            draw_status_bar(ed, "Error de memoria");
            return;
        }
        ed->buffer[0] = my_strdup("");
        if (!ed->buffer[0]) {
            free(ed->buffer);
            ed->buffer = NULL;
            draw_status_bar(ed, "Error de memoria");
            return;
        }
        ed->num_lines = 1;
        free(ed->filename);
        ed->filename = my_strdup(filename);
        ed->modified = 0;
        ed->cursor_x = 0;
        ed->cursor_y = 0;
        ed->top_line = 0;
        ed->left_col = 0;
        ed->readonly = !(st.st_mode & S_IWUSR);
        ed->original_mode = st.st_mode & 07777;
        ed->welcome_shown = 0;
        set_current_syntax(ed, ed->filename);
        reset_syntax_state(ed);
        invalidate_syntax_cache(ed);
        draw_status_bar(ed, error_msg);
        adjust_view(ed);
        return;
    }

    free_buffer(ed);
    ed->buffer = NULL;
    ed->num_lines = 0;
    char *line = NULL;
    size_t len = 0;
    while (read_file_line(fp, &line, &len) != -1) {
        char **new_buffer = realloc(ed->buffer, (ed->num_lines + 1) * sizeof(char *));
        if (!new_buffer) {
            free(line);
            fclose(fp);
            draw_status_bar(ed, "Error de memoria");
            return;
        }
        ed->buffer = new_buffer;
        char *dup = my_strdup(line);
        if (!dup) {
            free(line);
            fclose(fp);
            draw_status_bar(ed, "Error de memoria");
            return;
        }
        ed->buffer[ed->num_lines] = dup;
        ed->num_lines++;
        free(line);
        line = NULL;
    }
    free(line);
    fclose(fp);

    if (ed->num_lines == 0) {
        ed->buffer = malloc(sizeof(char *));
        if (!ed->buffer) {
            draw_status_bar(ed, "Error de memoria");
            return;
        }
        ed->buffer[0] = my_strdup("");
        if (!ed->buffer[0]) {
            free(ed->buffer);
            ed->buffer = NULL;
            draw_status_bar(ed, "Error de memoria");
            return;
        }
        ed->num_lines = 1;
    }

    free(ed->filename);
    ed->filename = my_strdup(filename);
    ed->modified = 0;
    ed->cursor_x = 0;
    ed->cursor_y = 0;
    ed->top_line = 0;
    ed->left_col = 0;
    ed->readonly = !(st.st_mode & S_IWUSR);
    ed->original_mode = st.st_mode & 07777;
    ed->welcome_shown = 0;

    set_current_syntax(ed, ed->filename);
    reset_syntax_state(ed);
    invalidate_syntax_cache(ed);

    adjust_view(ed);

    if (ed->readonly) {
        draw_status_bar(ed, "Archivo de solo lectura. Use ^O para guardar con otro nombre.");
    } else {
        draw_status_bar(ed, "Bienvenido a lye. Pulse ^G para ayuda.");
    }
}

int save_file(Editor *ed, const char *filename) {
    int need_chmod = 0;
    mode_t old_mode = 0644;

    struct stat st;
    if (stat(filename, &st) == 0) {
        old_mode = st.st_mode & 07777;
        if (!(old_mode & S_IWUSR)) {
            if (chmod(filename, old_mode | S_IWUSR) != 0) {
                draw_status_bar(ed, "No se pudo cambiar permisos para guardar.");
                return 0;
            }
            need_chmod = 1;
        }
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        if (need_chmod) chmod(filename, old_mode);
        draw_status_bar(ed, "Error al guardar archivo");
        return 0;
    }

    for (int i = 0; i < ed->num_lines; i++) {
        if (fputs(ed->buffer[i], fp) == EOF) {
            fclose(fp);
            if (need_chmod) chmod(filename, old_mode);
            draw_status_bar(ed, "Error al guardar");
            return 0;
        }
        if (i < ed->num_lines - 1) fputc('\n', fp);
    }
    if (ed->num_lines > 0 && ed->buffer[ed->num_lines-1][0] != '\0') {
        fputc('\n', fp);
    }
    if (fclose(fp) != 0) {
        if (need_chmod) chmod(filename, old_mode);
        draw_status_bar(ed, "Error al cerrar archivo");
        return 0;
    }

    if (need_chmod) {
        chmod(filename, old_mode);
        draw_status_bar(ed, "Archivo guardado (permisos restaurados)");
    } else {
        draw_status_bar(ed, "Archivo guardado");
    }

    char *new_filename = my_strdup(filename);
    if (new_filename) {
        free(ed->filename);
        ed->filename = new_filename;
        set_current_syntax(ed, ed->filename);
        reset_syntax_state(ed);
        invalidate_syntax_cache(ed);
    }
    ed->modified = 0;
    ed->readonly = 0;
    ed->welcome_shown = 0;
    return 1;
}

/* Funciones auxiliares para UTF-8 */
int prev_char_pos(const char *s, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

int next_char_pos(const char *s, int pos, int len) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

int visual_width_until(const char *line, int pos) {
    char *tmp = strndup(line, pos);
    wchar_t *wcs = calloc(pos + 1, sizeof(wchar_t));
    size_t n = mbstowcs(wcs, tmp, pos);
    int width = wcswidth(wcs, n);
    free(tmp);
    free(wcs);
    return width;
}

void invalidate_syntax_cache(Editor *ed) {
    if (ed->line_states) {
        for (int i = 0; i < ed->line_states_capacity; i++) {
            free(ed->line_states[i]);
        }
        free(ed->line_states);
        ed->line_states = NULL;
        ed->line_states_capacity = 0;
    }
}

void draw_screen(Editor *ed) {
    adjust_view(ed);
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    clearok(stdscr, TRUE);
    clear();

    attron(A_REVERSE);
    mvhline(0, 0, ' ', cols);
    char title[512];
    if (ed->filename) {
        char *safe_name = sanitize_string(ed->filename);
        snprintf(title, sizeof(title), " lye - Lynds Editor    %s%s%s", safe_name,
                 ed->modified ? " (modificado)" : "",
                 ed->readonly ? " [solo lectura]" : "");
        free(safe_name);
    } else {
        snprintf(title, sizeof(title), " lye - Lynds Editor    Búfer nuevo%s",
                 ed->modified ? " (modificado)" : "");
    }
    draw_truncated_utf8(0, 2, cols - 4, title);
    attroff(A_REVERSE);

    int total_lines = ed->num_lines + 1;
    int line_num_width = 1;
    int temp = total_lines;
    while (temp >= 10) { temp /= 10; line_num_width++; }
    line_num_width++;

    int edit_rows = rows - 3;
    int edit_cols = cols - line_num_width - 1;

    int segments[MAX_SEGMENTS * 4];
    int seg_count = 0;

    for (int i = 0; i < edit_rows; i++) {
        int line_index = ed->top_line + i;
        if (line_index >= total_lines) break;
        if (line_index < ed->num_lines) {
            attron(COLOR_PAIR(2));
            mvprintw(i + 1, 1, "%*d", line_num_width - 1, line_index + 1);
            attroff(COLOR_PAIR(2));

            char *line = ed->buffer[line_index];
            int len = (int)strlen(line);

            int has_left_marker = (ed->left_col > 0);
            int has_right_marker = (len > ed->left_col + edit_cols - (has_left_marker ? 1 : 0));

            int max_text_width = edit_cols - (has_left_marker ? 1 : 0) - (has_right_marker ? 1 : 0);
            int start_x = line_num_width + 1;
            int text_x = start_x + (has_left_marker ? 1 : 0);

            int visible_len = len - ed->left_col;
            if (visible_len < 0) visible_len = 0;
            if (visible_len > max_text_width) visible_len = max_text_width;

            if (has_left_marker) {
                attron(COLOR_PAIR(2));
                mvaddch(i + 1, start_x, '<');
                attroff(COLOR_PAIR(2));
            }

            if (visible_len > 0) {
                apply_syntax_to_line(ed, line_index, ed->left_col, visible_len,
                                     segments, &seg_count);
                int cur_x = text_x;
                for (int s = 0; s < seg_count; s += 4) {
                    int seg_start = segments[s];
                    int seg_end = segments[s + 1];
                    int pair = segments[s + 2];
                    int attrs = segments[s + 3];
                    if (seg_end > seg_start) {
                        if (pair && has_colors()) attron(COLOR_PAIR(pair));
                        if (attrs) attron(attrs);
                        mvaddnstr(i + 1, cur_x, line + seg_start, seg_end - seg_start);
                        if (attrs) attroff(attrs);
                        if (pair && has_colors()) attroff(COLOR_PAIR(pair));
                        cur_x += seg_end - seg_start;
                    }
                }
                if (seg_count == 0) {
                    mvaddnstr(i + 1, text_x, line + ed->left_col, visible_len);
                }
            }

            if (has_right_marker) {
                int right_marker_x = start_x + edit_cols - 1;
                attron(COLOR_PAIR(2));
                mvaddch(i + 1, right_marker_x, '>');
                attroff(COLOR_PAIR(2));
            }
        } else {
            attron(COLOR_PAIR(2));
            mvprintw(i + 1, 1, "%*d", line_num_width - 1, line_index + 1);
            attroff(COLOR_PAIR(2));
        }
    }

    attron(COLOR_PAIR(3));
    mvhline(rows - 2, 0, ' ', cols);
    if (ed->message[0] != '\0' && ed->msg_timeout > 0) {
        draw_truncated_utf8(rows - 2, 1, cols - 2, ed->message);
        ed->msg_timeout--;
        if (ed->msg_timeout == 0) ed->message[0] = '\0';
    } else if (ed->modified) {
        draw_truncated_utf8(rows - 2, 1, cols - 2, "Modificado");
    }
    attroff(COLOR_PAIR(3));

    attron(A_REVERSE);
    mvhline(rows - 1, 0, ' ', cols);
    const char *shortcuts;
    if (cols >= 90) {
        shortcuts = "^G Ayuda  ^S Guardar ^O Guardar como ^F Buscar  ^K Cortar ^X Salir ^/ Ir línea ";
    } else if (cols >= 60) {
        shortcuts = "^G Ayuda  ^S Guardar ^F Buscar  ^K Cortar ^X Salir";
    } else if (cols >= 30) {
        shortcuts = "^G Ayuda ^S Guardar ^X Salir";
    } else {
        shortcuts = "^G Ayuda";
    }
    draw_truncated_utf8(rows - 1, 1, cols - 2, shortcuts);
    attroff(A_REVERSE);

    int cursor_screen_y = ed->cursor_y - ed->top_line + 1;
    int cursor_screen_x;
    if (ed->cursor_y == ed->num_lines) {
        cursor_screen_x = line_num_width + 1;
    } else {
        int has_left_marker = (ed->left_col > 0);
        int visual_pos = visual_width_until(ed->buffer[ed->cursor_y], ed->cursor_x) -
        visual_width_until(ed->buffer[ed->cursor_y], ed->left_col);
        cursor_screen_x = line_num_width + 1 + (has_left_marker ? 1 : 0) + visual_pos;
    }
    if (cursor_screen_y >= 1 && cursor_screen_y <= edit_rows) {
        move(cursor_screen_y, cursor_screen_x);
    } else {
        move(1, line_num_width + 1);
    }
    refresh();
}

void draw_status_bar(Editor *ed, const char *msg) {
    strncpy(ed->message, msg, sizeof(ed->message) - 1);
    ed->message[sizeof(ed->message) - 1] = '\0';
    ed->msg_timeout = 50;
}

void show_help(Editor *ed) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    clearok(stdscr, TRUE);
    clear();
    attron(A_REVERSE);
    mvhline(0, 0, ' ', cols);
    draw_truncated_utf8(0, 2, cols - 4, " Ayuda de lye - Lynds Editor ");
    attroff(A_REVERSE);

    for (int i = 0; i < g_help_lines_count && i < rows - 2; i++) {
        draw_truncated_utf8(i + 2, 2, cols - 4, g_help_lines[i]);
    }
    refresh();

    timeout(100);
    int ch;
    while (1) {
        if (check_resize_ncurses(ed)) {
            getmaxyx(stdscr, rows, cols);
            clearok(stdscr, TRUE);
            clear();
            attron(A_REVERSE);
            mvhline(0, 0, ' ', cols);
            draw_truncated_utf8(0, 2, cols - 4, " Ayuda de lye - Lynds Editor ");
            attroff(A_REVERSE);
            for (int i = 0; i < g_help_lines_count && i < rows - 2; i++) {
                draw_truncated_utf8(i + 2, 2, cols - 4, g_help_lines[i]);
            }
            refresh();
            continue;
        }
        ch = getch();
        if (ch == ERR) continue;
        if (ch == KEY_RESIZE) { continue; }
        break;
    }
    draw_screen(ed);
}

void print_help_text(void) {
    for (int i = 0; i < g_help_lines_count; i++) {
        printf("%s\n", g_help_lines[i]);
    }
}

int read_line_from_user(Editor *ed, const char *prompt, char *buffer, int maxlen) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int pos = 0;
    buffer[0] = '\0';
    curs_set(1);

    attron(COLOR_PAIR(3));
    mvhline(rows - 2, 0, ' ', cols);
    draw_truncated_utf8(rows - 2, 1, cols - 2, prompt);
    attroff(COLOR_PAIR(3));
    int prompt_len = (int)strlen(prompt);
    move(rows - 2, 1 + prompt_len);
    refresh();

    timeout(100);

    while (1) {
        if (check_resize_ncurses(ed)) {
            getmaxyx(stdscr, rows, cols);
            attron(COLOR_PAIR(3));
            mvhline(rows - 2, 0, ' ', cols);
            draw_truncated_utf8(rows - 2, 1, cols - 2, prompt);
            attroff(COLOR_PAIR(3));
            prompt_len = (int)strlen(prompt);
            mvprintw(rows - 2, 1 + prompt_len, "%s", buffer);
            move(rows - 2, 1 + prompt_len + pos);
            refresh();
            continue;
        }

        wint_t wc;
        int res = get_wch(&wc);
        if (res == ERR) continue;

        if (res == KEY_CODE_YES) {
            int ch = (int)wc;
            if (ch == '\n' || ch == KEY_ENTER || ch == '\r') break;
            if (ch == KEY_CTRL('C')) {
                curs_set(1);
                draw_screen(ed);
                return ERR;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (pos > 0) {
                    int new_pos = prev_char_pos(buffer, pos);
                    buffer[new_pos] = '\0';
                    pos = new_pos;
                    mvhline(rows - 2, 1 + prompt_len, ' ', cols - 1 - prompt_len);
                    mvprintw(rows - 2, 1 + prompt_len, "%s", buffer);
                    move(rows - 2, 1 + prompt_len + pos);
                    refresh();
                }
                continue;
            }
            continue;
        } else {
            if (wc < 32) {
                int ch = (int)wc;
                if (ch == '\n' || ch == KEY_ENTER || ch == '\r') break;
                if (ch == KEY_CTRL('C')) {
                    curs_set(1);
                    draw_screen(ed);
                    return ERR;
                }
                if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                    if (pos > 0) {
                        int new_pos = prev_char_pos(buffer, pos);
                        buffer[new_pos] = '\0';
                        pos = new_pos;
                        mvhline(rows - 2, 1 + prompt_len, ' ', cols - 1 - prompt_len);
                        mvprintw(rows - 2, 1 + prompt_len, "%s", buffer);
                        move(rows - 2, 1 + prompt_len + pos);
                        refresh();
                    }
                    continue;
                }
                continue;
            }
            char mb[MB_CUR_MAX + 1];
            int len = wctomb(mb, (wchar_t)wc);
            if (len > 0) {
                if (pos + len < maxlen - 1) {  // dejar espacio para '\0'
                    memcpy(buffer + pos, mb, len);
                    pos += len;
                    buffer[pos] = '\0';
                    mvhline(rows - 2, 1 + prompt_len, ' ', cols - 1 - prompt_len);
                    mvprintw(rows - 2, 1 + prompt_len, "%s", buffer);
                    move(rows - 2, 1 + prompt_len + pos);
                    refresh();
                }
            }
        }
    }
    curs_set(1);
    draw_screen(ed);
    return OK;
}

int read_filename_with_default(Editor *ed, const char *prompt, char *buffer, int maxlen, const char *def) {
    char full_prompt[512];
    if (def && def[0]) {
        char *safe_def = sanitize_string(def);
        snprintf(full_prompt, sizeof(full_prompt), "%s [%s]: ", prompt, safe_def);
        free(safe_def);
    } else {
        snprintf(full_prompt, sizeof(full_prompt), "%s: ", prompt);
    }
    int result = read_line_from_user(ed, full_prompt, buffer, maxlen);
    if (result == OK && buffer[0] == '\0' && def && def[0]) {
        strncpy(buffer, def, maxlen);
        buffer[maxlen-1] = '\0';
    }
    return result;
}

void insert_string(Editor *ed, const char *s) {
    if (!s || !*s) return;
    push_history(ed);
    size_t len = strlen(s);
    if (ed->cursor_y == ed->num_lines) {
        char **new_buffer = realloc(ed->buffer, (ed->num_lines + 1) * sizeof(char *));
        if (!new_buffer) return;
        ed->buffer = new_buffer;
        char *new_line = my_strdup("");
        if (!new_line) return;
        ed->buffer[ed->num_lines] = new_line;
        ed->num_lines++;
        ed->cursor_y = ed->num_lines - 1;
        ed->cursor_x = 0;
        ed->left_col = 0;
    }
    size_t line_len = strlen(ed->buffer[ed->cursor_y]);
    if ((size_t)ed->cursor_x > line_len) ed->cursor_x = line_len;
    char *new_line = malloc(line_len + len + 1);
    if (!new_line) return;
    memcpy(new_line, ed->buffer[ed->cursor_y], ed->cursor_x);
    memcpy(new_line + ed->cursor_x, s, len);
    memcpy(new_line + ed->cursor_x + len, ed->buffer[ed->cursor_y] + ed->cursor_x,
           line_len - ed->cursor_x + 1);
    free(ed->buffer[ed->cursor_y]);
    ed->buffer[ed->cursor_y] = new_line;
    ed->cursor_x += len;
    ed->modified = 1;
    ed->welcome_shown = 1;
    invalidate_syntax_cache(ed);
    adjust_view(ed);
}

void delete_char(Editor *ed) {
    if (ed->cursor_y == ed->num_lines) {
        if (ed->num_lines > 0) {
            ed->cursor_y--;
            ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y]);
            ed->left_col = 0;
            adjust_view(ed);
        }
        return;
    }

    if (ed->cursor_x > 0) {
        push_history(ed);
        int start = prev_char_pos(ed->buffer[ed->cursor_y], ed->cursor_x);
        memmove(ed->buffer[ed->cursor_y] + start, ed->buffer[ed->cursor_y] + ed->cursor_x,
                strlen(ed->buffer[ed->cursor_y] + ed->cursor_x) + 1);
        ed->cursor_x = start;
        ed->modified = 1;
        ed->welcome_shown = 1;
        invalidate_syntax_cache(ed);
    } else if (ed->cursor_y > 0) {
        push_history(ed);
        size_t prev_len = strlen(ed->buffer[ed->cursor_y - 1]);
        size_t curr_len = strlen(ed->buffer[ed->cursor_y]);
        char *new_line = malloc(prev_len + curr_len + 1);
        if (!new_line) return;
        strcpy(new_line, ed->buffer[ed->cursor_y - 1]);
        strcat(new_line, ed->buffer[ed->cursor_y]);
        free(ed->buffer[ed->cursor_y - 1]);
        free(ed->buffer[ed->cursor_y]);
        ed->buffer[ed->cursor_y - 1] = new_line;
        for (int i = ed->cursor_y; i < ed->num_lines - 1; i++) ed->buffer[i] = ed->buffer[i + 1];
        ed->num_lines--;
        ed->cursor_y--;
        ed->cursor_x = (int)prev_len;
        ed->modified = 1;
        ed->welcome_shown = 1;
        ed->left_col = 0;
        invalidate_syntax_cache(ed);
    }
    adjust_view(ed);
}

void insert_newline(Editor *ed) {
    if (ed->cursor_y == ed->num_lines) {
        push_history(ed);
        char **new_buffer = realloc(ed->buffer, (ed->num_lines + 1) * sizeof(char *));
        if (!new_buffer) return;
        ed->buffer = new_buffer;
        char *empty = my_strdup("");
        if (!empty) return;
        ed->buffer[ed->num_lines] = empty;
        ed->num_lines++;
        ed->cursor_y = ed->num_lines;
        ed->cursor_x = 0;
        ed->modified = 1;
        ed->welcome_shown = 1;
        ed->left_col = 0;
        invalidate_syntax_cache(ed);
        adjust_view(ed);
        return;
    }
    push_history(ed);
    char *rest = my_strdup(ed->buffer[ed->cursor_y] + ed->cursor_x);
    if (!rest) return;
    ed->buffer[ed->cursor_y][ed->cursor_x] = '\0';
    char **new_buffer = realloc(ed->buffer, (ed->num_lines + 1) * sizeof(char *));
    if (!new_buffer) {
        free(rest);
        return;
    }
    ed->buffer = new_buffer;
    for (int i = ed->num_lines; i > ed->cursor_y + 1; i--) ed->buffer[i] = ed->buffer[i - 1];
    ed->buffer[ed->cursor_y + 1] = rest;
    ed->num_lines++;
    ed->cursor_y++;
    ed->cursor_x = 0;
    ed->modified = 1;
    ed->welcome_shown = 1;
    ed->left_col = 0;
    invalidate_syntax_cache(ed);
    adjust_view(ed);
}

void delete_current_line(Editor *ed) {
    if (ed->cursor_y == ed->num_lines) return;
    if (ed->num_lines <= 1) {
        push_history(ed);
        free(ed->buffer[0]);
        ed->buffer[0] = my_strdup("");
        if (!ed->buffer[0]) return;
        ed->cursor_x = 0;
        ed->modified = 1;
        ed->welcome_shown = 1;
        ed->left_col = 0;
        invalidate_syntax_cache(ed);
        return;
    }
    push_history(ed);
    free(ed->buffer[ed->cursor_y]);
    for (int i = ed->cursor_y; i < ed->num_lines - 1; i++) ed->buffer[i] = ed->buffer[i + 1];
    ed->num_lines--;
    if (ed->cursor_y >= ed->num_lines) ed->cursor_y = ed->num_lines;
    ed->cursor_x = 0;
    ed->modified = 1;
    ed->welcome_shown = 1;
    ed->left_col = 0;
    invalidate_syntax_cache(ed);
    adjust_view(ed);
}

void cut_line(Editor *ed) {
    if (ed->cursor_y == ed->num_lines) return;
    free_clipboard(ed);
    ed->clipboard = malloc(sizeof(char *));
    if (!ed->clipboard) return;
    ed->clipboard[0] = my_strdup(ed->buffer[ed->cursor_y]);
    if (!ed->clipboard[0]) {
        free(ed->clipboard);
        ed->clipboard = NULL;
        return;
    }
    ed->clipboard_lines = 1;
    delete_current_line(ed);
    draw_status_bar(ed, "Línea cortada");
}

void paste_clipboard(Editor *ed) {
    if (ed->clipboard_lines == 0) {
        draw_status_bar(ed, "Portapapeles vacío");
        return;
    }
    push_history(ed);
    int insert_after = (ed->cursor_y == ed->num_lines) ? ed->num_lines - 1 : ed->cursor_y;
    int old_num_lines = ed->num_lines;
    char **new_buffer = realloc(ed->buffer, (old_num_lines + ed->clipboard_lines) * sizeof(char *));
    if (!new_buffer) return;
    ed->buffer = new_buffer;
    for (int i = old_num_lines - 1; i > insert_after; i--) ed->buffer[i + ed->clipboard_lines] = ed->buffer[i];
    for (int i = 0; i < ed->clipboard_lines; i++) {
        ed->buffer[insert_after + 1 + i] = my_strdup(ed->clipboard[i]);
        if (!ed->buffer[insert_after + 1 + i]) {
            for (int j = 0; j < i; j++) free(ed->buffer[insert_after + 1 + j]);
            ed->num_lines = old_num_lines;
            return;
        }
    }
    ed->num_lines += ed->clipboard_lines;
    ed->cursor_y = insert_after + ed->clipboard_lines;
    ed->cursor_x = 0;
    ed->modified = 1;
    ed->welcome_shown = 1;
    ed->left_col = 0;
    invalidate_syntax_cache(ed);
    adjust_view(ed);
    draw_status_bar(ed, "Pegado");
}

void search(Editor *ed) {
    char pattern[256];
    if (read_line_from_user(ed, "Buscar: ", pattern, sizeof(pattern)) != OK) return;
    if (pattern[0] == '\0') return;
    strncpy(ed->last_search, pattern, sizeof(ed->last_search) - 1);
    ed->last_search[sizeof(ed->last_search) - 1] = '\0';
    int start_y = ed->cursor_y;
    int start_x = ed->cursor_x + 1;
    for (int i = 0; i < ed->num_lines; i++) {
        int line_idx = (start_y + i) % ed->num_lines;
        char *line = ed->buffer[line_idx];
        char *found = NULL;
        if (i == 0 && start_y == ed->cursor_y && start_y < ed->num_lines) {
            if ((size_t)start_x < strlen(line)) {
                found = strstr(line + start_x, pattern);
            }
        } else {
            found = strstr(line, pattern);
        }
        if (found) {
            ed->cursor_y = line_idx;
            ed->cursor_x = (int)(found - line);
            ed->left_col = 0;
            adjust_view(ed);
            draw_screen(ed);
            draw_status_bar(ed, "Coincidencia encontrada (^W para siguiente)");
            return;
        }
        start_x = 0;
    }
    draw_status_bar(ed, "No se encontró el texto");
}

void search_next(Editor *ed) {
    if (ed->last_search[0] == '\0') { search(ed); return; }
    char pattern[256];
    strncpy(pattern, ed->last_search, sizeof(pattern) - 1);
    pattern[sizeof(pattern) - 1] = '\0';
    int start_y = ed->cursor_y;
    int start_x = ed->cursor_x + 1;
    for (int i = 0; i < ed->num_lines; i++) {
        int line_idx = (start_y + i) % ed->num_lines;
        char *line = ed->buffer[line_idx];
        char *found = NULL;
        if (i == 0) {
            if ((size_t)start_x < strlen(line)) {
                found = strstr(line + start_x, pattern);
            }
        } else {
            found = strstr(line, pattern);
        }
        if (found) {
            ed->cursor_y = line_idx;
            ed->cursor_x = (int)(found - line);
            ed->left_col = 0;
            adjust_view(ed);
            draw_screen(ed);
            draw_status_bar(ed, "Coincidencia encontrada");
            return;
        }
        start_x = 0;
    }
    draw_status_bar(ed, "No hay más coincidencias");
}

void goto_line(Editor *ed) {
    char input[32];
    if (read_line_from_user(ed, "Ir a línea: ", input, sizeof(input)) != OK) return;
    char *endptr;
    long line_num = strtol(input, &endptr, 10);
    if (*endptr != '\0' || line_num < 1 || line_num > ed->num_lines + 1) {
        draw_status_bar(ed, "Número de línea inválido");
        return;
    }
    if (line_num == ed->num_lines + 1) ed->cursor_y = ed->num_lines;
    else ed->cursor_y = (int)line_num - 1;
    ed->cursor_x = 0;
    ed->left_col = 0;
    adjust_view(ed);
    draw_screen(ed);
}

void confirm_exit(Editor *ed) {
    if (ed->modified) {
        draw_status_bar(ed, "¿Guardar antes de salir? (s/n/C): ");
        draw_screen(ed);
        timeout(100);
        int ch;
        while (1) {
            if (check_resize_ncurses(ed)) {
                draw_status_bar(ed, "¿Guardar antes de salir? (s/n/C): ");
                draw_screen(ed);
                continue;
            }
            ch = getch();
            if (ch == ERR) continue;
            if (ch == KEY_RESIZE) { continue; }

            if (ch == 's' || ch == 'S') {
                if (ed->filename) {
                    if (save_file(ed, ed->filename)) {
                        cleanup();
                        exit(0);
                    }
                } else {
                    char filename[256] = "";
                    if (read_line_from_user(ed, "Nombre de archivo: ", filename, sizeof(filename)) == OK) {
                        if (save_file(ed, filename)) {
                            cleanup();
                            exit(0);
                        }
                    }
                }
            } else if (ch == 'n' || ch == 'N') {
                cleanup();
                exit(0);
            } else if (ch == 'c' || ch == 'C' || ch == 27) {
                draw_screen(ed);
                return;
            }
        }
    } else {
        cleanup();
        exit(0);
    }
}

void handle_input(Editor *ed, int ch) {
    switch (ch) {
        case KEY_CTRL('G'): show_help(ed); break;
        case KEY_CTRL_S:
            if (ed->filename) save_file(ed, ed->filename);
            else {
                char filename[256] = "";
                if (read_line_from_user(ed, "Nombre de archivo: ", filename, sizeof(filename)) == OK)
                    save_file(ed, filename);
            }
            break;
        case KEY_CTRL('O'): {
            char filename[256] = "";
            if (read_filename_with_default(ed, "Nombre de archivo", filename, sizeof(filename), ed->filename) == OK)
                save_file(ed, filename);
            break;
        }
        case KEY_CTRL('F'): search(ed); break;
        case KEY_CTRL('W'): search_next(ed); break;
        case KEY_CTRL('K'): cut_line(ed); break;
        case KEY_CTRL('U'): paste_clipboard(ed); break;
        case KEY_CTRL('Z'): undo(ed); break;
        case KEY_CTRL('Y'): redo(ed); break;
        case KEY_CTRL_SLASH: goto_line(ed); break;
        case KEY_CTRL('X'): confirm_exit(ed); break;
        case KEY_CTRL('L'):
            clearok(stdscr, TRUE);
            draw_screen(ed);
            break;
        case KEY_LEFT:
            if (ed->cursor_x > 0) ed->cursor_x = prev_char_pos(ed->buffer[ed->cursor_y], ed->cursor_x);
            else if (ed->cursor_y > 0) {
                ed->cursor_y--;
                ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y]);
                ed->left_col = 0;
            }
            adjust_view(ed);
            break;
        case KEY_RIGHT:
            if (ed->cursor_y < ed->num_lines && ed->cursor_x < (int)strlen(ed->buffer[ed->cursor_y]))
                ed->cursor_x = next_char_pos(ed->buffer[ed->cursor_y], ed->cursor_x, strlen(ed->buffer[ed->cursor_y]));
        else if (ed->cursor_y < ed->num_lines && ed->cursor_x == (int)strlen(ed->buffer[ed->cursor_y])) {
            ed->cursor_y++;
            ed->cursor_x = 0;
            ed->left_col = 0;
        }
        adjust_view(ed);
        break;
        case KEY_UP:
            if (ed->cursor_y > 0) {
                ed->cursor_y--;
                if (ed->cursor_y == ed->num_lines) ed->cursor_x = 0;
                else if ((size_t)ed->cursor_x > strlen(ed->buffer[ed->cursor_y])) ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y]);
                ed->left_col = 0;
            }
            adjust_view(ed);
            break;
        case KEY_DOWN:
            if (ed->cursor_y < ed->num_lines) {
                ed->cursor_y++;
                if (ed->cursor_y == ed->num_lines) ed->cursor_x = 0;
                else if ((size_t)ed->cursor_x > strlen(ed->buffer[ed->cursor_y])) ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y]);
                ed->left_col = 0;
            }
            adjust_view(ed);
            break;
        case KEY_HOME: ed->cursor_x = 0; ed->left_col = 0; adjust_view(ed); break;
        case KEY_END:
            if (ed->cursor_y < ed->num_lines) ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y]);
            else ed->cursor_x = 0;
            ed->left_col = 0;
        adjust_view(ed);
        break;
        case KEY_PPAGE:
            ed->cursor_y -= (getmaxy(stdscr) - 4);
            if (ed->cursor_y < 0) ed->cursor_y = 0;
            if (ed->cursor_y > ed->num_lines) ed->cursor_y = ed->num_lines;
            ed->left_col = 0;
        adjust_view(ed);
        break;
        case KEY_NPAGE:
            ed->cursor_y += (getmaxy(stdscr) - 4);
            if (ed->cursor_y > ed->num_lines) ed->cursor_y = ed->num_lines;
            ed->left_col = 0;
        adjust_view(ed);
        break;
        case KEY_BACKSPACE: case 127: case 8: delete_char(ed); break;
        case KEY_DC:
            if (ed->cursor_y < ed->num_lines && ed->cursor_x < (int)strlen(ed->buffer[ed->cursor_y])) {
                push_history(ed);
                int next = next_char_pos(ed->buffer[ed->cursor_y], ed->cursor_x, strlen(ed->buffer[ed->cursor_y]));
                memmove(ed->buffer[ed->cursor_y] + ed->cursor_x, ed->buffer[ed->cursor_y] + next,
                        strlen(ed->buffer[ed->cursor_y] + next) + 1);
                ed->modified = 1;
                ed->welcome_shown = 1;
                invalidate_syntax_cache(ed);
            } else if (ed->cursor_y < ed->num_lines - 1) {
                push_history(ed);
                size_t curr_len = strlen(ed->buffer[ed->cursor_y]);
                size_t next_len = strlen(ed->buffer[ed->cursor_y + 1]);
                char *merged = realloc(ed->buffer[ed->cursor_y], curr_len + next_len + 1);
                if (!merged) return;
                ed->buffer[ed->cursor_y] = merged;
                strcat(ed->buffer[ed->cursor_y], ed->buffer[ed->cursor_y + 1]);
                free(ed->buffer[ed->cursor_y + 1]);
                for (int i = ed->cursor_y + 1; i < ed->num_lines - 1; i++) ed->buffer[i] = ed->buffer[i + 1];
                ed->num_lines--;
                ed->modified = 1;
                ed->welcome_shown = 1;
                ed->left_col = 0;
                invalidate_syntax_cache(ed);
            }
            adjust_view(ed);
            break;
        case KEY_ENTER: case '\n': case '\r': insert_newline(ed); break;
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    int filename_index = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", VERSION);
            return 0;
        } else if (strcmp(argv[i], "--edition") == 0) {
            printf("%s\n", EDITION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_help_text();
            return 0;
        } else if (argv[i][0] != '-') {
            filename_index = i;
        }
    }

    Editor ed;
    memset(&ed, 0, sizeof(Editor));
    ed.history.max = 1000;
    ed.history.buffers = NULL;
    ed.history.num_lines = NULL;
    ed.history.cursor_x = NULL;
    ed.history.cursor_y = NULL;
    ed.history.count = 0;
    ed.history.index = -1;
    ed.clipboard = NULL;
    ed.clipboard_lines = 0;
    ed.filename = NULL;
    ed.modified = 0;
    ed.message[0] = '\0';
    ed.msg_timeout = 0;
    ed.last_search[0] = '\0';
    ed.readonly = 0;
    ed.welcome_shown = 0;
    ed.original_mode = 0644;
    ed.syntax_rule = NULL;
    ed.syntax_state = NULL;
    ed.line_states = NULL;
    ed.line_states_capacity = 0;

    init_curses();
    global_ed = &ed;

    init_syntax_highlighting();

    if (filename_index != -1) {
        load_file(&ed, argv[filename_index]);
    } else {
        ed.buffer = malloc(sizeof(char *));
        if (!ed.buffer) { cleanup(); return 1; }
        ed.buffer[0] = my_strdup("");
        if (!ed.buffer[0]) { free(ed.buffer); cleanup(); return 1; }
        ed.num_lines = 1;
        ed.filename = NULL;
        ed.readonly = 0;
        ed.welcome_shown = 0;
        set_current_syntax(&ed, NULL);
        reset_syntax_state(&ed);
        invalidate_syntax_cache(&ed);
        adjust_view(&ed);
        draw_status_bar(&ed, "Bienvenido a lye. Pulse ^G para ayuda. ^ es la tecla Ctrl");
    }

    draw_screen(&ed);

    timeout(100);

    while (1) {
        if (check_resize_ncurses(&ed)) continue;

        wint_t wc;
        int res = get_wch(&wc);
        if (res == ERR) continue;

        if (res == KEY_CODE_YES) {
            int ch = (int)wc;
            handle_input(&ed, ch);
        } else {
            if (wc < 32) {
                int ch = (int)wc;
                handle_input(&ed, ch);
            } else {
                char mb[MB_CUR_MAX + 1];
                int len = wctomb(mb, (wchar_t)wc);
                if (len > 0) {
                    mb[len] = '\0';
                    insert_string(&ed, mb);
                }
            }
        }
        draw_screen(&ed);
    }

    free_buffer(&ed);
    free_clipboard(&ed);
    free_history(&ed);
    free_syntax_rules();
    cleanup();
    return 0;
}
