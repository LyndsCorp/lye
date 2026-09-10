#ifndef NANORC_H
#define NANORC_H

#define _GNU_SOURCE

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>
#include <glob.h>
#include <limits.h>

#define MAX_SEGMENTS 512

typedef struct Editor Editor;

/* Los mensajes de depuración solo se emiten si LYE_DEBUG está en el
 * entorno. Sin este filtro, los fprintf(stderr, ...) del cargador de
 * sintaxis se escriben encima de la pantalla de ncurses y corrompen
 * la TUI. */
static int lye_debug_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = (getenv("LYE_DEBUG") != NULL);
    return cached;
}
#define LYE_DEBUG_LOG(...) \
do { if (lye_debug_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

    typedef struct {
        int fg;
        int bg;
        int attrs;
    } ColorSpec;

    typedef struct {
        ColorSpec color;
        regex_t regex;
        int is_icase;
    } ColorRule;

    typedef struct {
        ColorSpec color;
        regex_t start_regex;
        regex_t end_regex;
        int is_icase;
    } StartEndRule;

    typedef struct SyntaxRule {
        char *name;
        char *source_file;
        regex_t filename_regex;
        size_t filename_regex_len;
        ColorRule *color_rules;
        int color_rules_count;
        int color_rules_capacity;
        StartEndRule *startend_rules;
        int startend_rules_count;
        int startend_rules_capacity;
        struct SyntaxRule *next;
    } SyntaxRule;

    static SyntaxRule *g_syntax_list = NULL;
    static int g_color_pair_counter = 6;
    static int g_total_rules_loaded = 0;

    /* Definido en lye.c. Si es 0, set_current_syntax() no hace nada. */
    extern int g_use_nanorc;

    void init_syntax_highlighting(void);
    void set_current_syntax(Editor *ed, const char *filename);
    void reset_syntax_state(Editor *ed);
    void apply_syntax_to_line(Editor *ed, int line_index,
                              int start_col, int visible_len,
                              int *segments, int *seg_count);
    void free_syntax_rules(void);

    static void parse_color_spec(const char *spec, ColorSpec *cs);
    static int get_color_pair(ColorSpec *cs);
    static void add_color_rule(SyntaxRule *rule, const char *colorspec,
                               const char *regex, int icase);
    static void add_startend_rule(SyntaxRule *rule, const char *colorspec,
                                  const char *start, const char *end, int icase);
    static void parse_nanorc_file(const char *path);
    static char *trim(char *str);
    static int compile_regex(regex_t *preg, const char *pattern, int cflags);
    static int parse_hex_color(const char *hex);
    static int color_name_to_index(const char *name);

    /* ------------------------------------------------------------------------- */
    /* Implementación                                                            */
    /* ------------------------------------------------------------------------- */

    static char *trim(char *str) {
        while (isspace((unsigned char)*str)) str++;
        if (*str == '\0') return str;
        char *end = str + strlen(str) - 1;
        while (end > str && isspace((unsigned char)*end)) end--;
        *(end + 1) = '\0';
        return str;
    }

    static int parse_hex_color(const char *hex) {
        if (*hex == '#') hex++;
        int len = strlen(hex);
        if (len == 3 || len == 6) {
            char tmp[7] = {0};
            if (len == 3) {
                tmp[0] = hex[0]; tmp[1] = hex[0];
                tmp[2] = hex[1]; tmp[3] = hex[1];
                tmp[4] = hex[2]; tmp[5] = hex[2];
            } else {
                strncpy(tmp, hex, 6);
            }
            int r = (tmp[0] <= '9' ? tmp[0]-'0' : tolower(tmp[0])-'a'+10);
            int g = (tmp[2] <= '9' ? tmp[2]-'0' : tolower(tmp[2])-'a'+10);
            int b = (tmp[4] <= '9' ? tmp[4]-'0' : tolower(tmp[4])-'a'+10);
            if (r > 8) return COLOR_RED;
            if (g > 8) return COLOR_GREEN;
            if (b > 8) return COLOR_BLUE;
            if (r > 4 && g > 4) return COLOR_YELLOW;
            if (g > 4 && b > 4) return COLOR_CYAN;
            if (r > 4 && b > 4) return COLOR_MAGENTA;
            if (r > 4 || g > 4 || b > 4) return COLOR_WHITE;
            return COLOR_BLACK;
        }
        return -1;
    }

    static int color_name_to_index(const char *name) {
        if (strcmp(name, "black") == 0) return 0;
        if (strcmp(name, "red") == 0) return 1;
        if (strcmp(name, "green") == 0) return 2;
        if (strcmp(name, "yellow") == 0) return 3;
        if (strcmp(name, "blue") == 0) return 4;
        if (strcmp(name, "magenta") == 0) return 5;
        if (strcmp(name, "cyan") == 0) return 6;
        if (strcmp(name, "white") == 0) return 7;
        if (strcmp(name, "grey") == 0 || strcmp(name, "gray") == 0) return 8;
        if (strcmp(name, "pink") == 0 || strcmp(name, "rosy") == 0 ||
            strcmp(name, "mauve") == 0 || strcmp(name, "plum") == 0) return 5;
        if (strcmp(name, "purple") == 0 || strcmp(name, "slate") == 0) return 4;
        if (strcmp(name, "lagoon") == 0 || strcmp(name, "sea") == 0 ||
            strcmp(name, "sky") == 0 || strcmp(name, "teal") == 0) return 6;
        if (strcmp(name, "mint") == 0 || strcmp(name, "lime") == 0 ||
            strcmp(name, "sage") == 0) return 2;
        if (strcmp(name, "peach") == 0 || strcmp(name, "orange") == 0 ||
            strcmp(name, "latte") == 0 || strcmp(name, "sand") == 0 ||
            strcmp(name, "ocher") == 0 || strcmp(name, "tawny") == 0 ||
            strcmp(name, "brown") == 0) return 3;
        if (strcmp(name, "brick") == 0 || strcmp(name, "crimson") == 0) return 1;
        return -1;
    }

    static void parse_color_spec(const char *spec, ColorSpec *cs) {
        cs->fg = -1;
        cs->bg = -1;
        cs->attrs = 0;

        char *copy = strdup(spec);
        if (!copy) return;
        char *p = copy;
        char *fg_str = p;
        char *bg_str = NULL;

        char *comma = strchr(p, ',');
        if (comma) {
            *comma = '\0';
            bg_str = comma + 1;
        }

        char *fg = trim(fg_str);
        if (fg && *fg) {
            int bright = 0;
            if (strncmp(fg, "bright", 6) == 0 || strncmp(fg, "light", 5) == 0) {
                bright = 1;
                cs->attrs |= A_BOLD;
                fg += (strncmp(fg, "bright", 6) == 0) ? 6 : 5;
            } else if (strncmp(fg, "bold", 4) == 0) {
                cs->attrs |= A_BOLD;
                fg += 4;
            }
            while (*fg == ' ') fg++;

            if (*fg == '#') {
                int c = parse_hex_color(fg);
                if (c >= 0) cs->fg = c;
            } else {
                int color = color_name_to_index(fg);
                if (color >= 0) {
                    if (bright && color >= 0 && color <= 7) {
                        color += 8;
                    }
                    cs->fg = color;
                } else if (strcmp(fg, "normal") == 0) {
                    cs->fg = -1;
                    cs->attrs = 0;
                }
            }
        }

        if (bg_str) {
            char *bg = trim(bg_str);
            if (bg && *bg) {
                int bright = 0;
                if (strncmp(bg, "bright", 6) == 0 || strncmp(bg, "light", 5) == 0) {
                    bright = 1;
                    bg += (strncmp(bg, "bright", 6) == 0) ? 6 : 5;
                }
                while (*bg == ' ') bg++;

                if (*bg == '#') {
                    int c = parse_hex_color(bg);
                    if (c >= 0) cs->bg = c;
                } else {
                    int color = color_name_to_index(bg);
                    if (color >= 0) {
                        if (bright && color >= 0 && color <= 7) {
                            color += 8;
                        }
                        cs->bg = color;
                    } else if (strcmp(bg, "normal") == 0) {
                        cs->bg = -1;
                    }
                }
            }
        }
        free(copy);
    }

    static int get_color_pair(ColorSpec *cs) {
        if (!has_colors()) return 0;

        static int pairs[MAX_SEGMENTS][3];
        static int pair_count = 0;
        static int initialized = 0;

        if (!initialized) {
            for (int i = 0; i < MAX_SEGMENTS; i++) {
                pairs[i][0] = -1;
                pairs[i][1] = -1;
                pairs[i][2] = 0;
            }
            initialized = 1;
        }

        for (int i = 0; i < pair_count; i++) {
            if (pairs[i][0] == cs->fg && pairs[i][1] == cs->bg) {
                return pairs[i][2];
            }
        }
        if (pair_count < MAX_SEGMENTS && g_color_pair_counter < COLOR_PAIRS) {
            int pair_num = g_color_pair_counter++;
            init_pair(pair_num, cs->fg, cs->bg);
            pairs[pair_count][0] = cs->fg;
            pairs[pair_count][1] = cs->bg;
            pairs[pair_count][2] = pair_num;
            pair_count++;
            return pair_num;
        }
        return (pair_count > 0) ? pairs[0][2] : 0;
    }

    static int compile_regex(regex_t *preg, const char *pattern, int cflags) {
        int ret = regcomp(preg, pattern, cflags);
        if (ret == 0) return 0;

        size_t len = strlen(pattern);
        char *modified = NULL;
        size_t mod_cap = len * 4 + 32;
        modified = malloc(mod_cap);
        if (!modified) return ret;
        size_t j = 0;

        for (size_t i = 0; i < len; i++) {
            if (pattern[i] == '\\' && i + 1 < len) {
                char next = pattern[i + 1];
                const char *expansion = NULL;
                size_t exp_len = 0;
                switch (next) {
                    case '>': expansion = "([^[:alnum:]_]|$)"; exp_len = strlen(expansion); break;
                    case '<': expansion = "(^|[^[:alnum:]_])"; exp_len = strlen(expansion); break;
                    case 's': expansion = "[[:space:]]"; exp_len = strlen(expansion); break;
                    case 'S': expansion = "[^[:space:]]"; exp_len = strlen(expansion); break;
                    case 'w': expansion = "[[:alnum:]_]"; exp_len = strlen(expansion); break;
                    case 'B': expansion = "[^[:alnum:]_]"; exp_len = strlen(expansion); break;
                    case 'd': expansion = "[[:digit:]]"; exp_len = strlen(expansion); break;
                    case '"': expansion = "\""; exp_len = 1; break;
                    case '\\': expansion = "\\\\"; exp_len = 2; break;
                    default:
                        if (j + 2 >= mod_cap) {
                            mod_cap *= 2;
                            char *new_mod = realloc(modified, mod_cap);
                            if (!new_mod) { free(modified); return ret; }
                            modified = new_mod;
                        }
                        modified[j++] = pattern[i];
                        modified[j++] = pattern[i+1];
                        i++;
                        continue;
                }
                if (expansion) {
                    if (j + exp_len >= mod_cap) {
                        mod_cap = (j + exp_len + 1) * 2;
                        char *new_mod = realloc(modified, mod_cap);
                        if (!new_mod) { free(modified); return ret; }
                        modified = new_mod;
                    }
                    memcpy(modified + j, expansion, exp_len);
                    j += exp_len;
                    i++;
                    continue;
                }
            } else {
                if (j + 1 >= mod_cap) {
                    mod_cap *= 2;
                    char *new_mod = realloc(modified, mod_cap);
                    if (!new_mod) { free(modified); return ret; }
                    modified = new_mod;
                }
                modified[j++] = pattern[i];
            }
        }
        modified[j] = '\0';
        ret = regcomp(preg, modified, cflags);
        free(modified);
        return ret;
    }

    static void add_color_rule(SyntaxRule *rule, const char *colorspec,
                               const char *regex, int icase) {
        if (rule->color_rules_count >= rule->color_rules_capacity) {
            int new_cap = rule->color_rules_capacity ? rule->color_rules_capacity * 2 : 8;
            ColorRule *new_rules = realloc(rule->color_rules, new_cap * sizeof(ColorRule));
            if (!new_rules) return;
            rule->color_rules = new_rules;
            rule->color_rules_capacity = new_cap;
        }
        ColorRule *cr = &rule->color_rules[rule->color_rules_count];
        parse_color_spec(colorspec, &cr->color);
        int cflags = REG_EXTENDED;
        if (icase) cflags |= REG_ICASE;
        if (compile_regex(&cr->regex, regex, cflags) != 0) {
            LYE_DEBUG_LOG("[nanorc] Error compilando regex: %s\n", regex);
            return;
        }
        cr->is_icase = icase;
        rule->color_rules_count++;
                               }

                               static void add_startend_rule(SyntaxRule *rule, const char *colorspec,
                                                             const char *start, const char *end, int icase) {
                                   if (rule->startend_rules_count >= rule->startend_rules_capacity) {
                                       int new_cap = rule->startend_rules_capacity ? rule->startend_rules_capacity * 2 : 4;
                                       StartEndRule *new_rules = realloc(rule->startend_rules, new_cap * sizeof(StartEndRule));
                                       if (!new_rules) return;
                                       rule->startend_rules = new_rules;
                                       rule->startend_rules_capacity = new_cap;
                                   }
                                   StartEndRule *ser = &rule->startend_rules[rule->startend_rules_count];
                                   parse_color_spec(colorspec, &ser->color);
                                   int cflags = REG_EXTENDED;
                                   if (icase) cflags |= REG_ICASE;
                                   if (compile_regex(&ser->start_regex, start, cflags) != 0) return;
                                   if (compile_regex(&ser->end_regex, end, cflags) != 0) {
                                       regfree(&ser->start_regex);
                                       return;
                                   }
                                   ser->is_icase = icase;
                                   rule->startend_rules_count++;
                                                             }

                                                             static void parse_nanorc_file(const char *path) {
                                                                 FILE *fp = fopen(path, "r");
                                                                 if (!fp) return;

                                                                 char line[1024];
                                                                 SyntaxRule *current_rule = NULL;

                                                                 while (fgets(line, sizeof(line), fp)) {
                                                                     line[strcspn(line, "\n")] = '\0';
                                                                     char *trimmed = trim(line);
                                                                     if (*trimmed == '\0' || *trimmed == '#') continue;

                                                                     if (strncmp(trimmed, "syntax", 6) == 0) {
                                                                         char *p = trimmed + 6;
                                                                         while (*p && isspace((unsigned char)*p)) p++;
                                                                         if (*p == '\0') continue;

                                                                         char *name = NULL;
                                                                         if (*p == '"') {
                                                                             p++;
                                                                             char *start = p;
                                                                             while (*p && *p != '"') p++;
                                                                             if (*p == '"') {
                                                                                 name = strndup(start, p - start);
                                                                                 p++;
                                                                             } else {
                                                                                 name = strdup(start);
                                                                             }
                                                                         } else {
                                                                             char *start = p;
                                                                             while (*p && !isspace((unsigned char)*p)) p++;
                                                                             name = strndup(start, p - start);
                                                                         }
                                                                         while (*p && isspace((unsigned char)*p)) p++;

                                                                         char *filename_regex = NULL;
                                                                         if (*p == '"') {
                                                                             char *first_quote = p;
                                                                             char *last_quote = strrchr(p, '"');
                                                                             if (last_quote && last_quote > first_quote) {
                                                                                 filename_regex = strndup(first_quote + 1, last_quote - first_quote - 1);
                                                                             } else {
                                                                                 filename_regex = strdup(p + 1);
                                                                             }
                                                                         } else {
                                                                             filename_regex = strdup(p);
                                                                         }

                                                                         if (!name || !filename_regex || *filename_regex == '\0') {
                                                                             free(name);
                                                                             free(filename_regex);
                                                                             continue;
                                                                         }

                                                                         SyntaxRule *rule = calloc(1, sizeof(SyntaxRule));
                                                                         if (!rule) {
                                                                             free(name);
                                                                             free(filename_regex);
                                                                             continue;
                                                                         }
                                                                         rule->name = name;
                                                                         rule->source_file = strdup(path);
                                                                         rule->filename_regex_len = strlen(filename_regex);
                                                                         if (compile_regex(&rule->filename_regex, filename_regex, REG_EXTENDED) != 0) {
                                                                             free(name);
                                                                             free(filename_regex);
                                                                             free(rule->source_file);
                                                                             free(rule);
                                                                             continue;
                                                                         }
                                                                         free(filename_regex);

                                                                         rule->next = g_syntax_list;
                                                                         g_syntax_list = rule;
                                                                         current_rule = rule;
                                                                         g_total_rules_loaded++;
                                                                         continue;
                                                                     }

                                                                     if (!current_rule) continue;

                                                                     if (strncmp(trimmed, "color", 5) == 0 || strncmp(trimmed, "icolor", 6) == 0) {
                                                                         int icase = (trimmed[0] == 'i');
                                                                         char *p = trimmed + (icase ? 6 : 5);
                                                                         while (*p && isspace((unsigned char)*p)) p++;
                                                                         if (*p == '\0') continue;

                                                                         char *spec_start = p;
                                                                         while (*p && !isspace((unsigned char)*p)) p++;
                                                                         char *colorspec = strndup(spec_start, p - spec_start);

                                                                         while (*p && isspace((unsigned char)*p)) p++;

                                                                         if (strncmp(p, "start=", 6) == 0) {
                                                                             p += 6;
                                                                             char *start = NULL;
                                                                             char *end = NULL;
                                                                             if (*p == '"') {
                                                                                 char *start_begin = p + 1;
                                                                                 char *start_end = strchr(start_begin, '"');
                                                                                 if (start_end) {
                                                                                     start = strndup(start_begin, start_end - start_begin);
                                                                                     p = start_end + 1;
                                                                                 }
                                                                             }
                                                                             while (*p && isspace((unsigned char)*p)) p++;
                                                                             if (strncmp(p, "end=", 4) == 0) {
                                                                                 p += 4;
                                                                                 if (*p == '"') {
                                                                                     char *end_begin = p + 1;
                                                                                     char *end_end = strrchr(end_begin, '"');
                                                                                     if (end_end) {
                                                                                         end = strndup(end_begin, end_end - end_begin);
                                                                                     }
                                                                                 }
                                                                             }
                                                                             if (start && end) {
                                                                                 add_startend_rule(current_rule, colorspec, start, end, icase);
                                                                             }
                                                                             free(start);
                                                                             free(end);
                                                                         } else {
                                                                             if (*p == '"') {
                                                                                 char *first_quote = p;
                                                                                 char *last_quote = strrchr(p, '"');
                                                                                 if (last_quote && last_quote > first_quote) {
                                                                                     char *regex = strndup(first_quote + 1, last_quote - first_quote - 1);
                                                                                     add_color_rule(current_rule, colorspec, regex, icase);
                                                                                     free(regex);
                                                                                 }
                                                                             }
                                                                         }
                                                                         free(colorspec);
                                                                         continue;
                                                                     }
                                                                 }
                                                                 fclose(fp);
                                                             }

                                                             void init_syntax_highlighting(void) {
                                                                 if (!g_use_nanorc) return;

                                                                 glob_t globbuf;
                                                                 const char *patterns[] = {
                                                                     "/usr/share/nano/*.nanorc",
                                                                     "/usr/local/share/nano/*.nanorc",
                                                                     "/etc/nano/*.nanorc",
                                                                     "./*.nanorc",
                                                                     NULL
                                                                 };
                                                                 for (int p = 0; patterns[p]; p++) {
                                                                     if (glob(patterns[p], 0, NULL, &globbuf) == 0) {
                                                                         for (size_t i = 0; i < globbuf.gl_pathc; i++) {
                                                                             parse_nanorc_file(globbuf.gl_pathv[i]);
                                                                         }
                                                                         globfree(&globbuf);
                                                                     }
                                                                 }

                                                                 for (SyntaxRule *rule = g_syntax_list; rule; rule = rule->next) {
                                                                     if (strcmp(rule->name, "c") == 0 || strstr(rule->source_file, "c.nanorc") != NULL) {
                                                                         add_color_rule(rule, "brightblue", "\\*/", 0);
                                                                         break;
                                                                     }
                                                                 }

                                                                 LYE_DEBUG_LOG("[nanorc] Cargadas %d reglas de sintaxis\n", g_total_rules_loaded);
                                                             }

                                                             static SyntaxRule *find_syntax_for_filename(const char *filename) {
                                                                 if (!filename) return NULL;
                                                                 SyntaxRule *best = NULL;
                                                                 size_t best_len = 0;
                                                                 for (SyntaxRule *rule = g_syntax_list; rule; rule = rule->next) {
                                                                     if (regexec(&rule->filename_regex, filename, 0, NULL, 0) == 0) {
                                                                         if (rule->filename_regex_len > best_len) {
                                                                             best = rule;
                                                                             best_len = rule->filename_regex_len;
                                                                         }
                                                                     }
                                                                 }
                                                                 return best;
                                                             }

                                                             void set_current_syntax(Editor *ed, const char *filename) {
                                                                 if (ed->syntax_rule) {
                                                                     free(ed->syntax_state);
                                                                     ed->syntax_state = NULL;
                                                                     ed->syntax_rule = NULL;
                                                                 }

                                                                 if (!g_use_nanorc) {
                                                                     return;
                                                                 }

                                                                 SyntaxRule *rule = find_syntax_for_filename(filename);
                                                                 if (rule) {
                                                                     ed->syntax_rule = rule;
                                                                     ed->syntax_state = calloc(rule->startend_rules_count, sizeof(int));
                                                                     LYE_DEBUG_LOG("[nanorc] Sintaxis para '%s' -> regla '%s' (%d color rules, %d start/end rules) del archivo '%s'\n",
                                                                                   filename ? filename : "(null)", rule->name, rule->color_rules_count, rule->startend_rules_count, rule->source_file);
                                                                 } else {
                                                                     ed->syntax_rule = NULL;
                                                                     ed->syntax_state = NULL;
                                                                     LYE_DEBUG_LOG("[nanorc] No se encontró sintaxis para: %s\n", filename ? filename : "(null)");
                                                                 }
                                                             }

                                                             void reset_syntax_state(Editor *ed) {
                                                                 if (ed->syntax_state && ed->syntax_rule) {
                                                                     memset(ed->syntax_state, 0,
                                                                            ((SyntaxRule *)ed->syntax_rule)->startend_rules_count * sizeof(int));
                                                                 }
                                                             }

                                                             static void compute_line_states(Editor *ed, int line_index) {
                                                                 SyntaxRule *rule = (SyntaxRule *)ed->syntax_rule;
                                                                 if (!rule || !ed->syntax_state) return;
                                                                 if (line_index >= ed->line_states_capacity) {
                                                                     int new_cap = line_index + 1;
                                                                     int **new_states = realloc(ed->line_states, new_cap * sizeof(int *));
                                                                     if (!new_states) return;
                                                                     for (int i = ed->line_states_capacity; i < new_cap; i++) {
                                                                         new_states[i] = NULL;
                                                                     }
                                                                     ed->line_states = new_states;
                                                                     ed->line_states_capacity = new_cap;
                                                                 }
                                                                 if (ed->line_states[line_index]) return;

                                                                 int start_line = 0;
                                                                 int *prev_state = NULL;
                                                                 if (line_index > 0 && ed->line_states[line_index - 1]) {
                                                                     start_line = line_index;
                                                                     prev_state = ed->line_states[line_index - 1];
                                                                 } else {
                                                                     start_line = 0;
                                                                     prev_state = NULL;
                                                                 }

                                                                 int *state = malloc(rule->startend_rules_count * sizeof(int));
                                                                 if (!state) return;
                                                                 if (prev_state) {
                                                                     memcpy(state, prev_state, rule->startend_rules_count * sizeof(int));
                                                                 } else {
                                                                     memset(state, 0, rule->startend_rules_count * sizeof(int));
                                                                 }

                                                                 for (int l = start_line; l <= line_index; l++) {
                                                                     if (l >= ed->num_lines) break;
                                                                     const char *line = ed->buffer[l];
                                                                     if (!line) continue;
                                                                     for (int i = 0; i < rule->startend_rules_count; i++) {
                                                                         StartEndRule *ser = &rule->startend_rules[i];
                                                                         if (!state[i]) {
                                                                             regmatch_t match;
                                                                             if (regexec(&ser->start_regex, line, 1, &match, 0) == 0) {
                                                                                 state[i] = 1;
                                                                                 const char *after_start = line + match.rm_eo;
                                                                                 regmatch_t end_match;
                                                                                 if (regexec(&ser->end_regex, after_start, 1, &end_match, 0) == 0) {
                                                                                     state[i] = 0;
                                                                                 }
                                                                             }
                                                                         } else {
                                                                             regmatch_t match;
                                                                             if (regexec(&ser->end_regex, line, 1, &match, 0) == 0) {
                                                                                 state[i] = 0;
                                                                                 const char *after_end = line + match.rm_eo;
                                                                                 regmatch_t start_match;
                                                                                 if (regexec(&ser->start_regex, after_end, 1, &start_match, 0) == 0) {
                                                                                     state[i] = 1;
                                                                                 }
                                                                             }
                                                                         }
                                                                     }
                                                                     if (l < line_index) {
                                                                         if (!ed->line_states[l]) {
                                                                             ed->line_states[l] = malloc(rule->startend_rules_count * sizeof(int));
                                                                             if (ed->line_states[l]) {
                                                                                 memcpy(ed->line_states[l], state, rule->startend_rules_count * sizeof(int));
                                                                             }
                                                                         }
                                                                     }
                                                                 }
                                                                 ed->line_states[line_index] = state;
                                                             }

                                                             void apply_syntax_to_line(Editor *ed, int line_index,
                                                                                       int start_col, int visible_len,
                                                                                       int *segments, int *seg_count) {
                                                                 *seg_count = 0;
                                                                 SyntaxRule *rule = (SyntaxRule *)ed->syntax_rule;
                                                                 if (!rule || !ed->buffer || line_index < 0 || line_index >= ed->num_lines) {
                                                                     return;
                                                                 }

                                                                 compute_line_states(ed, line_index);

                                                                 char *line = ed->buffer[line_index];
                                                                 int line_len = strlen(line);
                                                                 if (start_col >= line_len) return;

                                                                 int end_col = start_col + visible_len;
                                                                 if (end_col > line_len) end_col = line_len;

                                                                 int *color_map = malloc((end_col - start_col + 1) * sizeof(int));
                                                                 int *attr_map = malloc((end_col - start_col + 1) * sizeof(int));
                                                                 if (!color_map || !attr_map) {
                                                                     free(color_map);
                                                                     free(attr_map);
                                                                     return;
                                                                 }
                                                                 for (int i = 0; i <= end_col - start_col; i++) {
                                                                     color_map[i] = 0;
                                                                     attr_map[i] = 0;
                                                                 }

                                                                 for (int i = 0; i < rule->color_rules_count; i++) {
                                                                     ColorRule *cr = &rule->color_rules[i];
                                                                     regmatch_t match;
                                                                     int offset = start_col;
                                                                     const char *search_start = line + offset;
                                                                     while (offset <= end_col && regexec(&cr->regex, search_start, 1, &match, 0) == 0) {
                                                                         int ms = offset + match.rm_so;
                                                                         int me = offset + match.rm_eo;
                                                                         if (ms >= end_col) break;
                                                                         if (me > end_col) me = end_col;
                                                                         int pair = get_color_pair(&cr->color);
                                                                         int attrs = cr->color.attrs;
                                                                         for (int pos = ms; pos < me; pos++) {
                                                                             if (pos >= start_col && pos < end_col) {
                                                                                 color_map[pos - start_col] = pair;
                                                                                 attr_map[pos - start_col] = attrs;
                                                                             }
                                                                         }
                                                                         if (match.rm_eo == 0) break;
                                                                         offset += match.rm_eo;
                                                                         search_start = line + offset;
                                                                     }
                                                                 }

                                                                 for (int i = 0; i < rule->startend_rules_count; i++) {
                                                                     StartEndRule *ser = &rule->startend_rules[i];
                                                                     int state = ed->line_states[line_index][i];
                                                                     int pos = start_col;

                                                                     if (!state) {
                                                                         regmatch_t match;
                                                                         if (regexec(&ser->start_regex, line, 1, &match, 0) == 0) {
                                                                             int ms = match.rm_so;
                                                                             int me = match.rm_eo;
                                                                             if (ms < start_col) {
                                                                                 state = 1;
                                                                                 ed->line_states[line_index][i] = 1;
                                                                                 pos = start_col;
                                                                             } else if (ms < end_col) {
                                                                                 if (me > end_col) me = end_col;
                                                                                 int pair = get_color_pair(&ser->color);
                                                                                 int attrs = ser->color.attrs;
                                                                                 for (int j = ms; j < me; j++) {
                                                                                     if (j >= start_col && j < end_col) {
                                                                                         color_map[j - start_col] = pair;
                                                                                         attr_map[j - start_col] = attrs;
                                                                                     }
                                                                                 }
                                                                                 state = 1;
                                                                                 ed->line_states[line_index][i] = 1;
                                                                                 pos = me;
                                                                             } else {
                                                                                 continue;
                                                                             }
                                                                         } else {
                                                                             continue;
                                                                         }
                                                                     }

                                                                     while (pos < end_col) {
                                                                         regmatch_t end_match;
                                                                         if (regexec(&ser->end_regex, line + pos, 1, &end_match, 0) == 0 &&
                                                                             pos + end_match.rm_so >= start_col && pos + end_match.rm_so < end_col) {
                                                                             int ms = pos + end_match.rm_so;
                                                                         int me = pos + end_match.rm_eo;
                                                                         if (me > end_col) me = end_col;
                                                                         int pair = get_color_pair(&ser->color);
                                                                             int attrs = ser->color.attrs;
                                                                             for (int j = pos; j < ms; j++) {
                                                                                 if (j >= start_col && j < end_col) {
                                                                                     color_map[j - start_col] = pair;
                                                                                     attr_map[j - start_col] = attrs;
                                                                                 }
                                                                             }
                                                                             for (int j = ms; j < me; j++) {
                                                                                 if (j >= start_col && j < end_col) {
                                                                                     color_map[j - start_col] = pair;
                                                                                     attr_map[j - start_col] = attrs;
                                                                                 }
                                                                             }
                                                                             state = 0;
                                                                             ed->line_states[line_index][i] = 0;
                                                                             pos = me;
                                                                             break;
                                                                             } else {
                                                                                 int pair = get_color_pair(&ser->color);
                                                                                 int attrs = ser->color.attrs;
                                                                                 for (int j = pos; j < end_col; j++) {
                                                                                     if (j >= start_col && j < end_col) {
                                                                                         color_map[j - start_col] = pair;
                                                                                         attr_map[j - start_col] = attrs;
                                                                                     }
                                                                                 }
                                                                                 ed->line_states[line_index][i] = 1;
                                                                                 break;
                                                                             }
                                                                     }
                                                                 }

                                                                 if (rule->name && strcmp(rule->name, "c") == 0) {
                                                                     for (int idx = start_col; idx < end_col - 1; idx++) {
                                                                         if (line[idx] == '*' && line[idx+1] == '/') {
                                                                             int pair = get_color_pair(&(ColorSpec){.fg=12, .bg=-1, .attrs=A_BOLD});
                                                                             for (int j = idx; j < idx+2 && j < end_col; j++) {
                                                                                 color_map[j - start_col] = pair;
                                                                                 attr_map[j - start_col] = A_BOLD;
                                                                             }
                                                                         }
                                                                     }
                                                                 }

                                                                 int current_pair = color_map[0];
                                                                 int current_attrs = attr_map[0];
                                                                 int current_start = start_col;
                                                                 for (int pos = start_col + 1; pos <= end_col; pos++) {
                                                                     int pair = (pos < end_col) ? color_map[pos - start_col] : -1;
                                                                     int attrs = (pos < end_col) ? attr_map[pos - start_col] : 0;
                                                                     if (pair != current_pair || attrs != current_attrs || pos == end_col) {
                                                                         segments[(*seg_count)++] = current_start;
                                                                         segments[(*seg_count)++] = (pos < end_col) ? pos : end_col;
                                                                         segments[(*seg_count)++] = current_pair;
                                                                         segments[(*seg_count)++] = current_attrs;
                                                                         if (*seg_count >= MAX_SEGMENTS * 4) break;
                                                                         current_start = pos;
                                                                         current_pair = pair;
                                                                         current_attrs = attrs;
                                                                     }
                                                                 }

                                                                 free(color_map);
                                                                 free(attr_map);
                                                                                       }

                                                                                       void free_syntax_rules(void) {
                                                                                           SyntaxRule *rule = g_syntax_list;
                                                                                           while (rule) {
                                                                                               SyntaxRule *next = rule->next;
                                                                                               for (int i = 0; i < rule->color_rules_count; i++) {
                                                                                                   regfree(&rule->color_rules[i].regex);
                                                                                               }
                                                                                               free(rule->color_rules);
                                                                                               for (int i = 0; i < rule->startend_rules_count; i++) {
                                                                                                   regfree(&rule->startend_rules[i].start_regex);
                                                                                                   regfree(&rule->startend_rules[i].end_regex);
                                                                                               }
                                                                                               free(rule->startend_rules);
                                                                                               free(rule->name);
                                                                                               free(rule->source_file);
                                                                                               regfree(&rule->filename_regex);
                                                                                               free(rule);
                                                                                               rule = next;
                                                                                           }
                                                                                           g_syntax_list = NULL;
                                                                                       }

                                                                                       #endif /* NANORC_H */
