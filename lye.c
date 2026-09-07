/*
 * lye - Lynds Editor
 * Editor de texto TUI en C11 inspirado en GNU nano.
 * Interfaz en español, con atajos y números de línea.
 */

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* Definiciones de teclas de control */
#define KEY_CTRL(x) ((x) & 0x1F)
#define KEY_CTRL_SLASH 0x1F   /* Ctrl+/ (0x1F) */

/* Estructura principal del editor */
typedef struct {
    char **buffer;          /* Líneas del archivo (sin '\n') */
    int num_lines;          /* Número de líneas en el buffer */
    int cursor_x, cursor_y; /* Posición del cursor (columna, fila) */
    int top_line;           /* Primera línea visible en pantalla */
    int left_col;           /* Primera columna visible */
    int modified;           /* Indica si hay cambios sin guardar */
    char *filename;         /* Nombre del archivo abierto (NULL si es búfer nuevo) */
    char message[256];      /* Mensaje en barra de estado */
    int msg_timeout;        /* Contador para borrar mensaje */
    /* Portapapeles interno */
    char **clipboard;
    int clipboard_lines;
    /* Historial para deshacer/rehacer */
    char ***history;        /* Pila de estados (cada estado es un buffer completo) */
    int *history_num_lines; /* Número de líneas de cada estado */
    int history_count;      /* Número de estados en la pila */
    int history_index;      /* Índice actual en la pila (posición del estado actual) */
    int max_history;        /* Límite máximo de estados */
    /* Búsqueda */
    char last_search[256];  /* Última búsqueda realizada */
} Editor;

/* Prototipos de funciones */
void init_curses(void);
void cleanup(void);
void handle_resize(int sig);
void draw_screen(Editor *ed);
void draw_status_bar(Editor *ed, const char *msg);
void show_help(Editor *ed);
void load_file(Editor *ed, const char *filename);
int save_file(Editor *ed, const char *filename);
void push_history(Editor *ed);
void undo(Editor *ed);
void redo(Editor *ed);
void free_buffer(Editor *ed);
void free_clipboard(Editor *ed);
void free_history(Editor *ed);
void insert_char(Editor *ed, char c);
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
char **duplicate_buffer(Editor *ed, int *num_lines_out);
char **duplicate_buffer_from_history(Editor *ed, int index, int *num_lines_out);
char *my_strdup(const char *s);
int read_file_line(FILE *fp, char **line, size_t *len);

/* Variables globales para señales */
Editor *global_ed = NULL;

/* Implementación de strdup (C11 estándar) */
char *my_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

/* Leer una línea de archivo de forma dinámica (sin getline) */
int read_file_line(FILE *fp, char **line, size_t *len) {
    size_t capacity = 128;
    size_t size = 0;
    char *buffer = malloc(capacity);
    if (!buffer) return -1;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') {
            break;
        }
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
    return size;
}

/* Inicializar ncurses */
void init_curses(void) {
    initscr();
    raw();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(1);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_WHITE, COLOR_BLUE);   /* Barra superior */
        init_pair(2, COLOR_YELLOW, COLOR_BLACK); /* Números de línea */
        init_pair(3, COLOR_CYAN, COLOR_BLACK);   /* Barra de estado */
        init_pair(4, COLOR_BLACK, COLOR_WHITE);  /* Barra de atajos */
        init_pair(5, COLOR_RED, COLOR_BLACK);    /* Mensajes de error */
    }
}

/* Restaurar terminal */
void cleanup(void) {
    endwin();
}

/* Manejar señal de redimensionamiento */
void handle_resize(int sig) {
    (void)sig; /* Evitar warning de parámetro no usado */
    if (global_ed) {
        endwin();
        refresh();
        clear();
        draw_screen(global_ed);
    }
}

/* Liberar buffer del editor */
void free_buffer(Editor *ed) {
    if (ed->buffer) {
        for (int i = 0; i < ed->num_lines; i++) {
            free(ed->buffer[i]);
        }
        free(ed->buffer);
        ed->buffer = NULL;
    }
    ed->num_lines = 0;
}

/* Liberar portapapeles */
void free_clipboard(Editor *ed) {
    if (ed->clipboard) {
        for (int i = 0; i < ed->clipboard_lines; i++) {
            free(ed->clipboard[i]);
        }
        free(ed->clipboard);
        ed->clipboard = NULL;
        ed->clipboard_lines = 0;
    }
}

/* Liberar historial */
void free_history(Editor *ed) {
    if (ed->history) {
        for (int i = 0; i < ed->history_count; i++) {
            for (int j = 0; j < ed->history_num_lines[i]; j++) {
                free(ed->history[i][j]);
            }
            free(ed->history[i]);
        }
        free(ed->history);
        free(ed->history_num_lines);
        ed->history = NULL;
        ed->history_num_lines = NULL;
        ed->history_count = 0;
        ed->history_index = -1;
    }
}

/* Copiar buffer completo a un nuevo estado para historial */
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

/* Guardar estado actual en historial (antes de modificar) */
void push_history(Editor *ed) {
    if (ed->history_count >= ed->max_history) {
        /* Eliminar el estado más antiguo */
        for (int j = 0; j < ed->history_num_lines[0]; j++) {
            free(ed->history[0][j]);
        }
        free(ed->history[0]);
        memmove(ed->history, ed->history + 1, (ed->history_count - 1) * sizeof(char **));
        memmove(ed->history_num_lines, ed->history_num_lines + 1, (ed->history_count - 1) * sizeof(int));
        ed->history_count--;
        if (ed->history_index > 0) ed->history_index--;
    }
    /* Agregar nuevo estado al final */
    int num_lines_copy;
    char **copy = duplicate_buffer(ed, &num_lines_copy);
    if (!copy) return;
    ed->history = realloc(ed->history, (ed->history_count + 1) * sizeof(char **));
    ed->history_num_lines = realloc(ed->history_num_lines, (ed->history_count + 1) * sizeof(int));
    ed->history[ed->history_count] = copy;
    ed->history_num_lines[ed->history_count] = num_lines_copy;
    ed->history_count++;
    ed->history_index = ed->history_count - 1;
    /* Limpiar rehacer (estados después del índice actual) */
    for (int i = ed->history_index + 1; i < ed->history_count; i++) {
        for (int j = 0; j < ed->history_num_lines[i]; j++) {
            free(ed->history[i][j]);
        }
        free(ed->history[i]);
    }
    ed->history_count = ed->history_index + 1;
}

/* Función auxiliar para duplicar buffer desde historial */
char **duplicate_buffer_from_history(Editor *ed, int index, int *num_lines_out) {
    char **copy = malloc(ed->history_num_lines[index] * sizeof(char *));
    if (!copy) return NULL;
    for (int i = 0; i < ed->history_num_lines[index]; i++) {
        copy[i] = my_strdup(ed->history[index][i]);
        if (!copy[i]) {
            for (int j = 0; j < i; j++) free(copy[j]);
            free(copy);
            return NULL;
        }
    }
    *num_lines_out = ed->history_num_lines[index];
    return copy;
}

/* Deshacer último cambio */
void undo(Editor *ed) {
    if (ed->history_index > 0) {
        ed->history_index--;
        free_buffer(ed);
        ed->buffer = duplicate_buffer_from_history(ed, ed->history_index, &ed->num_lines);
        ed->modified = 1;
        draw_screen(ed);
    } else {
        draw_status_bar(ed, "Nada que deshacer");
    }
}

/* Rehacer */
void redo(Editor *ed) {
    if (ed->history_index < ed->history_count - 1) {
        ed->history_index++;
        free_buffer(ed);
        ed->buffer = duplicate_buffer_from_history(ed, ed->history_index, &ed->num_lines);
        ed->modified = 1;
        draw_screen(ed);
    } else {
        draw_status_bar(ed, "Nada que rehacer");
    }
}

/* Cargar archivo en buffer */
void load_file(Editor *ed, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        draw_status_bar(ed, "Error al abrir archivo");
        return;
    }
    /* Liberar buffer anterior */
    free_buffer(ed);
    ed->buffer = NULL;
    ed->num_lines = 0;
    char *line = NULL;
    size_t len = 0;
    while (read_file_line(fp, &line, &len) != -1) {
        ed->buffer = realloc(ed->buffer, (ed->num_lines + 1) * sizeof(char *));
        ed->buffer[ed->num_lines] = my_strdup(line);
        ed->num_lines++;
        free(line);
        line = NULL;
    }
    free(line); /* por si acaso */
    fclose(fp);
    if (ed->num_lines == 0) {
        ed->buffer = malloc(sizeof(char *));
        ed->buffer[0] = my_strdup("");
        ed->num_lines = 1;
    }
    ed->filename = my_strdup(filename);
    ed->modified = 0;
    ed->cursor_x = 0;
    ed->cursor_y = 0;
    ed->top_line = 0;
    ed->left_col = 0;
}

/* Guardar buffer en archivo, asegurando salto de línea final */
int save_file(Editor *ed, const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        draw_status_bar(ed, "Error al guardar archivo");
        return 0;
    }
    for (int i = 0; i < ed->num_lines; i++) {
        fputs(ed->buffer[i], fp);
        if (i < ed->num_lines - 1) {
            fputc('\n', fp);
        }
    }
    /* Asegurar salto de línea final si el buffer no termina con nueva línea */
    if (ed->num_lines > 0 && ed->buffer[ed->num_lines-1][0] != '\0') {
        fputc('\n', fp);
    } else if (ed->num_lines == 0) {
        fputc('\n', fp);
    }
    fclose(fp);
    ed->filename = my_strdup(filename);
    ed->modified = 0;
    draw_status_bar(ed, "Archivo guardado");
    return 1;
}

/* Dibujar pantalla completa */
void draw_screen(Editor *ed) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();

    /* Barra superior */
    attron(A_REVERSE);
    mvhline(0, 0, ' ', cols);
    char title[256];
    if (ed->filename) {
        snprintf(title, sizeof(title), " lye - Lynds Editor    %s%s", ed->filename, ed->modified ? " (modificado)" : "");
    } else {
        snprintf(title, sizeof(title), " lye - Lynds Editor    Búfer nuevo%s", ed->modified ? " (modificado)" : "");
    }
    mvprintw(0, 2, "%s", title);
    attroff(A_REVERSE);

    /* Área de edición */
    int line_num_width = 1;
    int temp = ed->num_lines;
    while (temp >= 10) {
        temp /= 10;
        line_num_width++;
    }
    line_num_width++; /* espacio extra */

    int edit_rows = rows - 3;
    int edit_cols = cols - line_num_width - 1;

    for (int i = 0; i < edit_rows && (ed->top_line + i) < ed->num_lines; i++) {
        int line_index = ed->top_line + i;
        attron(COLOR_PAIR(2));
        mvprintw(i + 1, 1, "%*d", line_num_width - 1, line_index + 1);
        attroff(COLOR_PAIR(2));
        char *line = ed->buffer[line_index];
        int len = (int)strlen(line);
        int display_len = len - ed->left_col;
        if (display_len < 0) display_len = 0;
        if (display_len > edit_cols) display_len = edit_cols;
        if (display_len > 0) {
            mvaddnstr(i + 1, line_num_width + 1, line + ed->left_col, display_len);
        }
    }

    /* Barra de estado */
    attron(COLOR_PAIR(3));
    mvhline(rows - 2, 0, ' ', cols);
    if (ed->message[0] != '\0' && ed->msg_timeout > 0) {
        mvprintw(rows - 2, 1, "%s", ed->message);
        ed->msg_timeout--;
        if (ed->msg_timeout == 0) ed->message[0] = '\0';
    } else if (ed->modified) {
        mvprintw(rows - 2, 1, "Modificado");
    } else {
        mvprintw(rows - 2, 1, " ");
    }
    attroff(COLOR_PAIR(3));

    /* Barra de atajos */
    attron(A_REVERSE);
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 1, "^G Ayuda  ^O Guardar ^F Buscar  ^K Cortar  ^U Pegar  ^Z Deshacer ^Y Rehacer ^/ Ir línea ^X Salir");
    attroff(A_REVERSE);

    int cursor_screen_y = ed->cursor_y - ed->top_line + 1;
    int cursor_screen_x = ed->cursor_x - ed->left_col + line_num_width + 1;
    if (cursor_screen_y >= 1 && cursor_screen_y <= edit_rows) {
        move(cursor_screen_y, cursor_screen_x);
    } else {
        move(1, line_num_width + 1);
    }
    refresh();
}

/* Mostrar mensaje en barra de estado */
void draw_status_bar(Editor *ed, const char *msg) {
    strncpy(ed->message, msg, sizeof(ed->message) - 1);
    ed->message[sizeof(ed->message) - 1] = '\0';
    ed->msg_timeout = 50;
}

/* Mostrar pantalla de ayuda */
void show_help(Editor *ed) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    attron(A_REVERSE);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 2, " Ayuda de lye - Lynds Editor ");
    attroff(A_REVERSE);

    const char *help_lines[] = {
        "Atajos de teclado principales:",
        "",
        "^G   Mostrar esta ayuda",
        "^O   Guardar archivo",
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
        "Presione cualquier tecla para volver..."
    };
    int n = sizeof(help_lines) / sizeof(help_lines[0]);
    for (int i = 0; i < n && i < rows - 2; i++) {
        mvprintw(i + 2, 2, "%s", help_lines[i]);
    }
    refresh();
    getch();
    draw_screen(ed);
}

/* Leer una línea de texto del usuario en la barra de estado */
int read_line_from_user(Editor *ed, const char *prompt, char *buffer, int maxlen) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    echo();
    curs_set(1);
    attron(COLOR_PAIR(3));
    mvhline(rows - 2, 0, ' ', cols);
    mvprintw(rows - 2, 1, "%s", prompt);
    attroff(COLOR_PAIR(3));
    move(rows - 2, 1 + strlen(prompt));
    refresh();
    int result = getnstr(buffer, maxlen - 1);
    noecho();
    curs_set(1);
    draw_screen(ed);
    return result;
}

/* Insertar carácter en posición actual */
void insert_char(Editor *ed, char c) {
    push_history(ed);
    size_t line_len = strlen(ed->buffer[ed->cursor_y]);
    if ((size_t)ed->cursor_x > line_len) ed->cursor_x = (int)line_len;
    char *new_line = malloc(line_len + 2);
    if (!new_line) return;
    memcpy(new_line, ed->buffer[ed->cursor_y], ed->cursor_x);
    new_line[ed->cursor_x] = c;
    memcpy(new_line + ed->cursor_x + 1, ed->buffer[ed->cursor_y] + ed->cursor_x, line_len - ed->cursor_x + 1);
    free(ed->buffer[ed->cursor_y]);
    ed->buffer[ed->cursor_y] = new_line;
    ed->cursor_x++;
    ed->modified = 1;
    if (ed->cursor_x >= ed->left_col + (getmaxx(stdscr) - 6)) {
        ed->left_col = ed->cursor_x - (getmaxx(stdscr) - 6) + 1;
    }
}

/* Eliminar carácter bajo cursor o anterior (Backspace) */
void delete_char(Editor *ed) {
    if (ed->cursor_x > 0) {
        push_history(ed);
        size_t line_len = strlen(ed->buffer[ed->cursor_y]);
        memmove(ed->buffer[ed->cursor_y] + ed->cursor_x - 1, ed->buffer[ed->cursor_y] + ed->cursor_x, line_len - ed->cursor_x + 1);
        ed->cursor_x--;
        ed->modified = 1;
    } else if (ed->cursor_y > 0) {
        push_history(ed);
        size_t prev_len = strlen(ed->buffer[ed->cursor_y - 1]);
        size_t curr_len = strlen(ed->buffer[ed->cursor_y]);
        char *new_line = malloc(prev_len + curr_len + 1);
        strcpy(new_line, ed->buffer[ed->cursor_y - 1]);
        strcat(new_line, ed->buffer[ed->cursor_y]);
        free(ed->buffer[ed->cursor_y - 1]);
        free(ed->buffer[ed->cursor_y]);
        ed->buffer[ed->cursor_y - 1] = new_line;
        for (int i = ed->cursor_y; i < ed->num_lines - 1; i++) {
            ed->buffer[i] = ed->buffer[i + 1];
        }
        ed->num_lines--;
        ed->cursor_y--;
        ed->cursor_x = (int)prev_len;
        ed->modified = 1;
    }
}

/* Insertar nueva línea (Enter) */
void insert_newline(Editor *ed) {
    push_history(ed);
    char *rest = my_strdup(ed->buffer[ed->cursor_y] + ed->cursor_x);
    ed->buffer[ed->cursor_y][ed->cursor_x] = '\0';
    ed->buffer = realloc(ed->buffer, (ed->num_lines + 1) * sizeof(char *));
    for (int i = ed->num_lines; i > ed->cursor_y + 1; i--) {
        ed->buffer[i] = ed->buffer[i - 1];
    }
    ed->buffer[ed->cursor_y + 1] = rest;
    ed->num_lines++;
    ed->cursor_y++;
    ed->cursor_x = 0;
    ed->modified = 1;
}

/* Eliminar línea completa actual */
void delete_current_line(Editor *ed) {
    if (ed->num_lines <= 1) {
        push_history(ed);
        free(ed->buffer[0]);
        ed->buffer[0] = my_strdup("");
        ed->cursor_x = 0;
        ed->modified = 1;
        return;
    }
    push_history(ed);
    free(ed->buffer[ed->cursor_y]);
    for (int i = ed->cursor_y; i < ed->num_lines - 1; i++) {
        ed->buffer[i] = ed->buffer[i + 1];
    }
    ed->num_lines--;
    if (ed->cursor_y >= ed->num_lines) ed->cursor_y = ed->num_lines - 1;
    ed->cursor_x = 0;
    ed->modified = 1;
}

/* Cortar línea actual al portapapeles */
void cut_line(Editor *ed) {
    free_clipboard(ed);
    ed->clipboard = malloc(sizeof(char *));
    ed->clipboard[0] = my_strdup(ed->buffer[ed->cursor_y]);
    ed->clipboard_lines = 1;
    delete_current_line(ed);
    draw_status_bar(ed, "Línea cortada");
}

/* Pegar contenido del portapapeles en posición actual */
void paste_clipboard(Editor *ed) {
    if (ed->clipboard_lines == 0) {
        draw_status_bar(ed, "Portapapeles vacío");
        return;
    }
    push_history(ed);
    int old_num_lines = ed->num_lines;
    ed->buffer = realloc(ed->buffer, (old_num_lines + ed->clipboard_lines) * sizeof(char *));
    for (int i = old_num_lines - 1; i > ed->cursor_y; i--) {
        ed->buffer[i + ed->clipboard_lines] = ed->buffer[i];
    }
    for (int i = 0; i < ed->clipboard_lines; i++) {
        ed->buffer[ed->cursor_y + 1 + i] = my_strdup(ed->clipboard[i]);
    }
    ed->num_lines += ed->clipboard_lines;
    ed->modified = 1;
    draw_status_bar(ed, "Pegado");
}

/* Buscar texto */
void search(Editor *ed) {
    char pattern[256];
    if (read_line_from_user(ed, "Buscar: ", pattern, sizeof(pattern)) != OK) return;
    if (pattern[0] == '\0') return;
    strncpy(ed->last_search, pattern, sizeof(ed->last_search));
    int start_y = ed->cursor_y;
    int start_x = ed->cursor_x + 1;
    for (int i = 0; i < ed->num_lines; i++) {
        int line_idx = (start_y + i) % ed->num_lines;
        char *line = ed->buffer[line_idx];
        char *found = NULL;
        if (i == 0 && start_y == ed->cursor_y) {
            if ((size_t)start_x < strlen(line)) {
                found = strstr(line + start_x, pattern);
            }
        } else {
            found = strstr(line, pattern);
        }
        if (found) {
            ed->cursor_y = line_idx;
            ed->cursor_x = (int)(found - line);
            if (ed->cursor_y < ed->top_line) ed->top_line = ed->cursor_y;
            if (ed->cursor_y >= ed->top_line + (getmaxy(stdscr) - 3)) ed->top_line = ed->cursor_y - (getmaxy(stdscr) - 4);
            if (ed->cursor_x < ed->left_col) ed->left_col = ed->cursor_x;
            if (ed->cursor_x >= ed->left_col + (getmaxx(stdscr) - 6)) ed->left_col = ed->cursor_x - (getmaxx(stdscr) - 6) + 1;
            draw_screen(ed);
            draw_status_bar(ed, "Coincidencia encontrada (^W para siguiente)");
            return;
        }
        start_x = 0;
    }
    draw_status_bar(ed, "No se encontró el texto");
}

/* Buscar siguiente */
void search_next(Editor *ed) {
    if (ed->last_search[0] == '\0') {
        search(ed);
        return;
    }
    char pattern[256];
    strncpy(pattern, ed->last_search, sizeof(pattern));
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
            if (ed->cursor_y < ed->top_line) ed->top_line = ed->cursor_y;
            if (ed->cursor_y >= ed->top_line + (getmaxy(stdscr) - 3)) ed->top_line = ed->cursor_y - (getmaxy(stdscr) - 4);
            if (ed->cursor_x < ed->left_col) ed->left_col = ed->cursor_x;
            if (ed->cursor_x >= ed->left_col + (getmaxx(stdscr) - 6)) ed->left_col = ed->cursor_x - (getmaxx(stdscr) - 6) + 1;
            draw_screen(ed);
            draw_status_bar(ed, "Coincidencia encontrada");
            return;
        }
        start_x = 0;
    }
    draw_status_bar(ed, "No hay más coincidencias");
}

/* Ir a línea específica */
void goto_line(Editor *ed) {
    char input[32];
    if (read_line_from_user(ed, "Ir a línea: ", input, sizeof(input)) != OK) return;
    int line_num = atoi(input);
    if (line_num < 1 || line_num > ed->num_lines) {
        draw_status_bar(ed, "Número de línea inválido");
        return;
    }
    ed->cursor_y = line_num - 1;
    ed->cursor_x = 0;
    if (ed->cursor_y < ed->top_line) ed->top_line = ed->cursor_y;
    if (ed->cursor_y >= ed->top_line + (getmaxy(stdscr) - 3)) ed->top_line = ed->cursor_y - (getmaxy(stdscr) - 4);
    draw_screen(ed);
}

/* Confirmar salida */
void confirm_exit(Editor *ed) {
    if (ed->modified) {
        draw_status_bar(ed, "¿Guardar antes de salir? (s/n/c): ");
        int ch = getch();
        if (ch == 's' || ch == 'S') {
            char filename[256];
            if (ed->filename) {
                if (save_file(ed, ed->filename)) {
                    cleanup();
                    exit(0);
                }
            } else {
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
        } else {
            draw_screen(ed);
        }
    } else {
        cleanup();
        exit(0);
    }
}

/* Manejar entrada de teclado */
void handle_input(Editor *ed, int ch) {
    switch (ch) {
        case KEY_CTRL('G'): show_help(ed); break;
        case KEY_CTRL('O'): {
            char filename[256];
            if (ed->filename) {
                save_file(ed, ed->filename);
            } else {
                if (read_line_from_user(ed, "Nombre de archivo: ", filename, sizeof(filename)) == OK) {
                    save_file(ed, filename);
                }
            }
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
        case KEY_CTRL('L'): clear(); draw_screen(ed); break;
        case KEY_LEFT:
            if (ed->cursor_x > 0) {
                ed->cursor_x--;
            } else if (ed->cursor_y > 0) {
                ed->cursor_y--;
                ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y]);
            }
            break;
        case KEY_RIGHT:
            if ((size_t)ed->cursor_x < strlen(ed->buffer[ed->cursor_y])) {
                ed->cursor_x++;
            } else if (ed->cursor_y < ed->num_lines - 1) {
                ed->cursor_y++;
                ed->cursor_x = 0;
            }
            break;
        case KEY_UP:
            if (ed->cursor_y > 0) {
                ed->cursor_y--;
                if ((size_t)ed->cursor_x > strlen(ed->buffer[ed->cursor_y])) {
                    ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y]);
                }
            }
            break;
        case KEY_DOWN:
            if (ed->cursor_y < ed->num_lines - 1) {
                ed->cursor_y++;
                if ((size_t)ed->cursor_x > strlen(ed->buffer[ed->cursor_y])) {
                    ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y]);
                }
            }
            break;
        case KEY_HOME: ed->cursor_x = 0; break;
        case KEY_END: ed->cursor_x = (int)strlen(ed->buffer[ed->cursor_y]); break;
        case KEY_PPAGE:
            ed->cursor_y -= (getmaxy(stdscr) - 4);
            if (ed->cursor_y < 0) ed->cursor_y = 0;
            break;
        case KEY_NPAGE:
            ed->cursor_y += (getmaxy(stdscr) - 4);
            if (ed->cursor_y >= ed->num_lines) ed->cursor_y = ed->num_lines - 1;
            break;
        case KEY_BACKSPACE:
        case 127:
        case 8:
            delete_char(ed);
            break;
        case KEY_DC:
            if ((size_t)ed->cursor_x < strlen(ed->buffer[ed->cursor_y])) {
                push_history(ed);
                memmove(ed->buffer[ed->cursor_y] + ed->cursor_x, ed->buffer[ed->cursor_y] + ed->cursor_x + 1, strlen(ed->buffer[ed->cursor_y] + ed->cursor_x));
                ed->modified = 1;
            } else if (ed->cursor_y < ed->num_lines - 1) {
                push_history(ed);
                size_t curr_len = strlen(ed->buffer[ed->cursor_y]);
                size_t next_len = strlen(ed->buffer[ed->cursor_y + 1]);
                ed->buffer[ed->cursor_y] = realloc(ed->buffer[ed->cursor_y], curr_len + next_len + 1);
                strcat(ed->buffer[ed->cursor_y], ed->buffer[ed->cursor_y + 1]);
                free(ed->buffer[ed->cursor_y + 1]);
                for (int i = ed->cursor_y + 1; i < ed->num_lines - 1; i++) {
                    ed->buffer[i] = ed->buffer[i + 1];
                }
                ed->num_lines--;
                ed->modified = 1;
            }
            break;
        case KEY_ENTER:
        case '\n':
        case '\r':
            insert_newline(ed);
            break;
        default:
            if (isprint(ch)) {
                insert_char(ed, ch);
            }
            break;
    }
    /* Ajustar scroll */
    if (ed->cursor_y < ed->top_line) ed->top_line = ed->cursor_y;
    if (ed->cursor_y >= ed->top_line + (getmaxy(stdscr) - 3)) ed->top_line = ed->cursor_y - (getmaxy(stdscr) - 4);
    if (ed->cursor_x < ed->left_col) ed->left_col = ed->cursor_x;
    if (ed->cursor_x >= ed->left_col + (getmaxx(stdscr) - 6)) ed->left_col = ed->cursor_x - (getmaxx(stdscr) - 6) + 1;
    draw_screen(ed);
}

int main(int argc, char *argv[]) {
    Editor ed;
    memset(&ed, 0, sizeof(Editor));
    ed.max_history = 1000;
    ed.history = NULL;
    ed.history_num_lines = NULL;
    ed.history_count = 0;
    ed.history_index = -1;
    ed.clipboard = NULL;
    ed.clipboard_lines = 0;
    ed.filename = NULL;
    ed.modified = 0;
    ed.message[0] = '\0';
    ed.msg_timeout = 0;
    ed.last_search[0] = '\0';

    init_curses();
    global_ed = &ed;
    signal(SIGWINCH, handle_resize);

    if (argc > 1) {
        load_file(&ed, argv[1]);
    } else {
        ed.buffer = malloc(sizeof(char *));
        ed.buffer[0] = my_strdup("");
        ed.num_lines = 1;
        ed.filename = NULL;
    }

    draw_screen(&ed);
    int ch;
    while (1) {
        ch = getch();
        handle_input(&ed, ch);
    }

    free_buffer(&ed);
    free_clipboard(&ed);
    free_history(&ed);
    cleanup();
    return 0;
}
