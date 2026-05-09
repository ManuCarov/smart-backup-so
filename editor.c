/*
 * editor.c
 * ---------------------------------------------------------------------------
 * Implementación del editor de texto con Gap Buffer.
 *
 * Módulos internos:
 *   1. Gap Buffer  — estructura de datos del editor (malloc/realloc/free)
 *   2. I/O Binario — guardar/cargar en formato .ebso con FileHeader packed
 *   3. mmap Load   — carga de texto plano via mapeo en memoria
 *   4. Editor TUI  — interfaz interactiva en terminal (termios raw mode)
 *
 * Proyecto Parcial 3: Editor de Archivos con Optimización de Bus I/O
 * Sistemas Operativos - EAFIT
 * ---------------------------------------------------------------------------
 */

#define _GNU_SOURCE   /* Habilita madvise, MADV_SEQUENTIAL en sys/mman.h */

#include "editor.h"
#include "smart_copy.h"

#include <stdlib.h>     /* malloc, calloc, realloc, free                  */
#include <string.h>     /* memcpy, memmove, memset, strlen, strncpy       */
#include <fcntl.h>      /* open, O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC    */
#include <unistd.h>     /* read, write, close, STDIN/STDOUT_FILENO        */
#include <sys/stat.h>   /* stat, struct stat                              */
#include <sys/mman.h>   /* mmap, munmap, PROT_READ, MAP_PRIVATE           */
#include <sys/ioctl.h>  /* ioctl, TIOCGWINSZ, struct winsize              */
#include <errno.h>      /* errno, ENOENT                                  */
#include <time.h>       /* time()                                         */
#include <termios.h>    /* tcgetattr, tcsetattr — modo raw de terminal    */
#include <stdio.h>      /* snprintf, fprintf (solo para UI/mensajes)      */
#include <stdint.h>     /* uint8_t, uint32_t, etc.                        */

/* Verificación estática del tamaño de las structs empaquetadas */
_Static_assert(sizeof(FileHeader) == 24,
    "FileHeader debe ocupar exactamente 24 bytes sin padding");
_Static_assert(sizeof(StyleEntry) == 10,
    "StyleEntry debe ocupar exactamente 10 bytes sin padding");

/* =========================================================================
 * SECCIÓN 1: GAP BUFFER — Gestión de memoria dinámica
 * ========================================================================= */

/**
 * gb_create — Reserva memoria para el Gap Buffer usando calloc.
 * calloc inicializa a cero, evitando garbage en el buffer de texto.
 */
GapBuffer *gb_create(void) {
    GapBuffer *gb = (GapBuffer *)calloc(1, sizeof(GapBuffer));
    if (!gb) return NULL;

    /* Reservar el buffer inicial. calloc pone todo a 0. */
    gb->buf = (char *)calloc(GAP_INITIAL_SIZE, 1);
    if (!gb->buf) {
        free(gb);
        return NULL;
    }

    gb->buf_size  = GAP_INITIAL_SIZE;
    gb->gap_start = 0;
    gb->gap_end   = GAP_INITIAL_SIZE; /* el gap ocupa todo el buffer al inicio */
    gb->modified  = 0;
    gb->filepath[0] = '\0';

    return gb;
}

/**
 * gb_destroy — Libera toda la memoria dinámica del Gap Buffer.
 * Orden: primero el buffer de datos, luego la estructura.
 */
void gb_destroy(GapBuffer *gb) {
    if (!gb) return;
    if (gb->buf) {
        free(gb->buf);
        gb->buf = NULL;
    }
    free(gb);
}

/**
 * gb_grow_gap — Expande el gap cuando se ha llenado.
 * Usa realloc para extender el buffer y memmove para preservar el texto
 * post-gap en su nueva posición al final del buffer expandido.
 *
 * Antes: [pre][gap=0][post]    buf_size=N
 * Después: [pre][gap=GROW_SIZE][post]    buf_size=N+GROW_SIZE
 */
static int gb_grow_gap(GapBuffer *gb) {
    size_t new_size     = gb->buf_size + GAP_GROW_SIZE;
    size_t post_len     = gb->buf_size - gb->gap_end;   /* bytes después del gap */

    char *new_buf = (char *)realloc(gb->buf, new_size);
    if (!new_buf) return -1;

    gb->buf = new_buf;

    /* Mover el texto post-gap al final del nuevo buffer ampliado */
    if (post_len > 0) {
        memmove(gb->buf + new_size - post_len,
                gb->buf + gb->gap_end,
                post_len);
    }

    gb->gap_end  = new_size - post_len;
    gb->buf_size = new_size;

    return 0;
}

/**
 * gb_move_gap_to — Mueve el gap a la posición `pos` en el texto visible.
 * Esta operación mueve físicamente los caracteres del buffer para que
 * el gap quede justo en `pos`.
 */
static void gb_move_gap_to(GapBuffer *gb, size_t pos) {
    if (pos == gb->gap_start) return;

    size_t gap_len = gb->gap_end - gb->gap_start;

    if (pos < gb->gap_start) {
        /* Mover cursor a la izquierda: desplazar texto pre-gap hacia la derecha */
        size_t move_len = gb->gap_start - pos;
        memmove(gb->buf + gb->gap_end - move_len,
                gb->buf + pos,
                move_len);
        gb->gap_start = pos;
        gb->gap_end   = pos + gap_len;
    } else {
        /* Mover cursor a la derecha: desplazar texto post-gap hacia la izquierda */
        /* pos aquí es en texto visible, necesitamos convertir a índice en buf */
        size_t real_pos = pos + gap_len; /* posición real en el buffer */
        size_t move_len = real_pos - gb->gap_end;
        memmove(gb->buf + gb->gap_start,
                gb->buf + gb->gap_end,
                move_len);
        gb->gap_start = pos;
        gb->gap_end   = pos + gap_len;
    }
}

/**
 * gb_insert_char — Inserta un carácter en la posición del cursor.
 * Si el gap está lleno, primero lo expande con realloc.
 */
int gb_insert_char(GapBuffer *gb, char c) {
    if (!gb) return -1;

    /* Si el gap está lleno, crecer */
    if (gb->gap_start == gb->gap_end) {
        if (gb_grow_gap(gb) != 0) return -1;
    }

    gb->buf[gb->gap_start++] = c;
    gb->modified = 1;
    return 0;
}

/**
 * gb_insert_string — Inserta `len` bytes de `str` en el cursor.
 */
int gb_insert_string(GapBuffer *gb, const char *str, size_t len) {
    if (!gb || !str) return -1;
    for (size_t i = 0; i < len; i++) {
        if (gb_insert_char(gb, str[i]) != 0) return -1;
    }
    return 0;
}

/**
 * gb_delete_before — Elimina el carácter ANTES del cursor (Backspace).
 * Simplemente retrocede gap_start — el carácter queda en el gap (basura).
 */
int gb_delete_before(GapBuffer *gb) {
    if (!gb || gb->gap_start == 0) return -1;
    gb->gap_start--;
    gb->modified = 1;
    return 0;
}

/**
 * gb_delete_after — Elimina el carácter DESPUÉS del cursor (Delete).
 * Avanza gap_end — el carácter queda cubierto por el gap (basura).
 */
int gb_delete_after(GapBuffer *gb) {
    if (!gb || gb->gap_end >= gb->buf_size) return -1;
    gb->gap_end++;
    gb->modified = 1;
    return 0;
}

/**
 * gb_move_cursor — Mueve el cursor `delta` posiciones.
 * Positivo = derecha, negativo = izquierda.
 */
int gb_move_cursor(GapBuffer *gb, int delta) {
    if (!gb) return -1;

    long new_pos = (long)gb->gap_start + delta;
    size_t text_len = gb_text_length(gb);

    if (new_pos < 0) new_pos = 0;
    if ((size_t)new_pos > text_len) new_pos = (long)text_len;

    gb_move_gap_to(gb, (size_t)new_pos);
    return 0;
}

void gb_cursor_home(GapBuffer *gb) {
    if (gb) gb_move_gap_to(gb, 0);
}

void gb_cursor_end(GapBuffer *gb) {
    if (gb) gb_move_gap_to(gb, gb_text_length(gb));
}

/**
 * gb_text_length — Tamaño del texto visible (sin el gap).
 */
size_t gb_text_length(const GapBuffer *gb) {
    if (!gb) return 0;
    return gb->buf_size - (gb->gap_end - gb->gap_start);
}

/**
 * gb_to_string — Copia el texto visible a una cadena C contigua.
 * El llamador DEBE liberar el puntero con free().
 */
char *gb_to_string(const GapBuffer *gb) {
    if (!gb) return NULL;

    size_t len = gb_text_length(gb);
    char *result = (char *)malloc(len + 1);
    if (!result) return NULL;

    /* Copiar pre-gap + post-gap, saltando el gap */
    memcpy(result, gb->buf, gb->gap_start);
    memcpy(result + gb->gap_start,
           gb->buf + gb->gap_end,
           gb->buf_size - gb->gap_end);
    result[len] = '\0';

    return result;
}

/**
 * gb_print — Imprime el documento a stdout usando write() (sin stdio).
 */
void gb_print(const GapBuffer *gb) {
    if (!gb) return;
    /* Escribir pre-gap */
    if (gb->gap_start > 0)
        write(STDOUT_FILENO, gb->buf, gb->gap_start);
    /* Escribir post-gap */
    size_t post_len = gb->buf_size - gb->gap_end;
    if (post_len > 0)
        write(STDOUT_FILENO, gb->buf + gb->gap_end, post_len);
}

/* =========================================================================
 * SECCIÓN 2: I/O BINARIO — Guardar y cargar formato .ebso
 * Usa las funciones de compresión del motor de backup (no static).
 * ========================================================================= */

/**
 * gb_save — Guarda el documento en formato .ebso usando syscalls POSIX.
 *
 * Pipeline (ningún byte de texto plano va al disco):
 *   texto en RAM → compresión en RAM → write() del header → write() del payload
 *
 * El FileHeader se escribe con write(fd, &header, sizeof(FileHeader)).
 * Gracias a __attribute__((packed)), sizeof(FileHeader) == 24 exactamente.
 */
int gb_save(GapBuffer *gb, const char *path, int algo) {
    if (!gb || !path) return -1;

    /* 1. Obtener texto plano */
    size_t text_len = gb_text_length(gb);
    char *text = gb_to_string(gb);
    if (!text) return -1;

    /* 2. Reservar buffer de compresión con malloc (peor caso: 2x el tamaño) */
    size_t comp_buf_size = text_len * 2 + 1024;
    char *comp_buf = (char *)malloc(comp_buf_size);
    if (!comp_buf) { free(text); return -1; }

    /* 3. Comprimir en memoria — ningún byte plano toca el disco */
    size_t comp_size = 0;
    if (algo == ALG_RLE) {
        comp_size = compress_rle_buffer(text, text_len, comp_buf);
    } else if (algo == ALG_LZ77 || algo == ALG_TURBOQUANT_LZ) {
        comp_size = compress_lz77_buffer(text, text_len, comp_buf);
    } else {
        /* algo == 0: copiar sin comprimir (para benchmarks) */
        memcpy(comp_buf, text, text_len);
        comp_size = text_len;
    }

    free(text); /* liberar texto plano — ya no lo necesitamos */

    /* 4. Construir cabecera binaria empaquetada */
    FileHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, EBSO_MAGIC, 4);
    header.version         = EBSO_VERSION;
    header.algo            = (uint8_t)algo;
    header.flags           = 0;
    header.original_size   = (uint32_t)text_len;
    header.compressed_size = (uint32_t)comp_size;
    header.timestamp       = (uint64_t)time(NULL);

    /* 5. Abrir/crear archivo destino con syscall open() */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(comp_buf);
        return -1;
    }

    /* 6. Escribir header (24 bytes exactos gracias a packed) */
    ssize_t wr = write(fd, &header, sizeof(FileHeader));
    if (wr != (ssize_t)sizeof(FileHeader)) {
        close(fd);
        free(comp_buf);
        return -1;
    }

    /* 7. Escribir payload comprimido */
    ssize_t written = 0;
    while ((size_t)written < comp_size) {
        ssize_t n = write(fd, comp_buf + written, comp_size - (size_t)written);
        if (n < 0) { close(fd); free(comp_buf); return -1; }
        written += n;
    }

    close(fd);
    free(comp_buf);

    gb->modified = 0;
    strncpy(gb->filepath, path, MAX_PATH_EDITOR - 1);
    gb->filepath[MAX_PATH_EDITOR - 1] = '\0';

    return 0;
}

/**
 * gb_load — Carga un documento .ebso desde disco.
 * Lee el FileHeader, verifica el magic, descomprime el payload.
 */
int gb_load(GapBuffer *gb, const char *path) {
    if (!gb || !path) return -1;

    /* 1. Abrir con syscall */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    /* 2. Leer y verificar la cabecera */
    FileHeader header;
    if (read(fd, &header, sizeof(FileHeader)) != (ssize_t)sizeof(FileHeader)) {
        close(fd);
        return -1;
    }

    if (memcmp(header.magic, EBSO_MAGIC, 4) != 0) {
        close(fd); /* No es un archivo .ebso */
        return -2;
    }

    /* 3. Reservar buffer para datos comprimidos */
    char *comp_buf = (char *)malloc(header.compressed_size);
    if (!comp_buf) { close(fd); return -1; }

    /* 4. Leer payload comprimido */
    ssize_t total_read = 0;
    while ((size_t)total_read < header.compressed_size) {
        ssize_t n = read(fd, comp_buf + total_read,
                         header.compressed_size - (size_t)total_read);
        if (n <= 0) { close(fd); free(comp_buf); return -1; }
        total_read += n;
    }
    close(fd);

    /* 5. Reservar buffer de descompresión con malloc */
    char *text_buf = (char *)malloc(header.original_size + 1);
    if (!text_buf) { free(comp_buf); return -1; }

    /* 6. Descomprimir en memoria */
    size_t decomp_size = 0;
    if (header.algo == ALG_RLE) {
        decomp_size = decompress_rle_buffer(comp_buf, header.compressed_size, text_buf);
    } else if (header.algo == ALG_LZ77 || header.algo == ALG_TURBOQUANT_LZ) {
        decomp_size = decompress_lz77_buffer(comp_buf, header.compressed_size, text_buf);
    } else {
        memcpy(text_buf, comp_buf, header.compressed_size);
        decomp_size = header.compressed_size;
    }
    free(comp_buf);

    text_buf[decomp_size] = '\0';

    /* 7. Insertar texto en el Gap Buffer */
    gb_cursor_home(gb);
    if (gb_insert_string(gb, text_buf, decomp_size) != 0) {
        free(text_buf);
        return -1;
    }
    gb_cursor_home(gb);

    free(text_buf);
    gb->modified = 0;
    strncpy(gb->filepath, path, MAX_PATH_EDITOR - 1);
    gb->filepath[MAX_PATH_EDITOR - 1] = '\0';

    return 0;
}

/**
 * gb_load_plaintext_mmap — Carga texto plano usando mmap().
 *
 * JUSTIFICACIÓN DEL DISEÑO vs read() con buffer:
 *   - read() copia datos: disco→caché_kernel→buffer_usuario (2 copias)
 *   - mmap() mapea la caché del kernel directo al espacio del proceso (0 copias extra)
 *   - Para archivos grandes, mmap reduce la presión en el bus I/O al evitar
 *     la copia intermedia. El kernel gestiona page faults bajo demanda.
 *   - strace mostrará page_fault en lugar de read() calls repetitivos.
 *
 * En el benchmark (--perf) se mide la diferencia empírica.
 */
int gb_load_plaintext_mmap(GapBuffer *gb, const char *path) {
    if (!gb || !path) return -1;

    /* 1. Obtener tamaño del archivo */
    struct stat st;
    if (stat(path, &st) == -1) return -1;
    if (st.st_size == 0) return 0; /* archivo vacío, no hacer nada */

    /* 2. Abrir el archivo solo para lectura */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    /*
     * 3. Mapear el archivo en memoria virtual del proceso.
     *    PROT_READ  — solo lectura
     *    MAP_PRIVATE — cambios no se propagan al archivo (copy-on-write)
     *    Offset 0   — desde el inicio del archivo
     */
    void *mapped = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* El fd puede cerrarse; el mapeo permanece válido */

    if (mapped == MAP_FAILED) return -1;

    /*
     * 4. Insertar el texto mapeado en el Gap Buffer.
     * madvise() con MADV_SEQUENTIAL le indica al kernel que leemos
     * secuencialmente, optimizando el read-ahead de páginas.
     */
    madvise(mapped, (size_t)st.st_size, MADV_SEQUENTIAL);

    int ret = gb_insert_string(gb, (const char *)mapped, (size_t)st.st_size);

    /* 5. Liberar el mapeo */
    munmap(mapped, (size_t)st.st_size);

    if (ret == 0) {
        gb_cursor_home(gb);
        gb->modified = 0;
        strncpy(gb->filepath, path, MAX_PATH_EDITOR - 1);
        gb->filepath[MAX_PATH_EDITOR - 1] = '\0';
    }

    return ret;
}

/* =========================================================================
 * SECCIÓN 3: EDITOR INTERACTIVO — Interfaz TUI en terminal
 * ========================================================================= */

/* Códigos de teclas especiales */
#define KEY_UP        1000
#define KEY_DOWN      1001
#define KEY_LEFT      1002
#define KEY_RIGHT     1003
#define KEY_HOME      1004
#define KEY_END       1005
#define KEY_DEL       1006
#define KEY_PAGE_UP   1007
#define KEY_PAGE_DOWN 1008
#define EDITOR_CTRL(k)  ((k) & 0x1f)  /* Ctrl+letra → valor ASCII 1-26 */

static struct termios s_orig_termios;
static int s_raw_mode_active = 0;

static void editor_restore_terminal(void) {
    if (s_raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_orig_termios);
        write(STDOUT_FILENO, "\033[?25h", 6); /* Mostrar cursor */
        s_raw_mode_active = 0;
    }
}

static int editor_enable_raw(void) {
    if (tcgetattr(STDIN_FILENO, &s_orig_termios) == -1) return -1;

    struct termios raw = s_orig_termios;
    /* Desactivar: eco, modo canónico, señales de control, procesamiento de salida */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=  (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;  /* read() bloquea hasta leer 1 byte */
    raw.c_cc[VTIME] = 0;  /* sin timeout */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;
    s_raw_mode_active = 1;
    return 0;
}

static void editor_get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        *rows = 24; *cols = 80; /* fallback */
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    }
}

/**
 * editor_read_key — Lee una tecla del terminal, decodificando secuencias ESC.
 */
static int editor_read_key(void) {
    char c;
    while (read(STDIN_FILENO, &c, 1) != 1) {
        if (errno != EAGAIN) return -1;
    }

    if (c != '\033') return (unsigned char)c;

    /* Leer posible secuencia de escape CSI */
    char seq[4] = {0};
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\033';
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': case '7': return KEY_HOME;
                    case '3':           return KEY_DEL;
                    case '4': case '8': return KEY_END;
                    case '5':           return KEY_PAGE_UP;
                    case '6':           return KEY_PAGE_DOWN;
                }
            }
        } else {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
        }
    }

    return '\033';
}

/*
 * Buffer acumulador para reducir llamadas a write() al pintar la pantalla.
 * Pintar caracter por caracter generaría miles de syscalls write().
 * Acumular todo en un buffer y hacer un write() grande reduce los
 * context switches kernel↔user drásticamente (visible en strace -c).
 */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} PaintBuf;

static int pb_init(PaintBuf *pb) {
    pb->cap  = 4096;
    pb->len  = 0;
    pb->data = (char *)malloc(pb->cap);
    return pb->data ? 0 : -1;
}

static void pb_free(PaintBuf *pb) {
    free(pb->data);
    pb->data = NULL;
    pb->len = pb->cap = 0;
}

static void pb_append(PaintBuf *pb, const char *s, size_t n) {
    if (pb->len + n > pb->cap) {
        pb->cap = pb->len + n + 4096;
        char *tmp = (char *)realloc(pb->data, pb->cap);
        if (!tmp) return;
        pb->data = tmp;
    }
    memcpy(pb->data + pb->len, s, n);
    pb->len += n;
}

static void pb_append_str(PaintBuf *pb, const char *s) {
    pb_append(pb, s, strlen(s));
}

static void pb_flush(PaintBuf *pb) {
    /* Un único write() para toda la pantalla — mínimos context switches */
    if (pb->len > 0) write(STDOUT_FILENO, pb->data, pb->len);
    pb->len = 0;
}

/**
 * editor_render — Redibuja la pantalla completa acumulando en PaintBuf.
 * Divide el texto en líneas y posiciona el cursor con ANSI codes.
 */
static void editor_render(GapBuffer *gb, int algo, int scroll_row,
                           int term_rows, int term_cols, PaintBuf *pb) {
    char tmp[256];
    pb->len = 0;

    /* Ocultar cursor durante el redibujado (evita parpadeo) */
    pb_append_str(pb, "\033[?25l");

    /* Obtener texto completo con posición del cursor */
    char *text      = gb_to_string(gb);
    size_t text_len = gb_text_length(gb);
    size_t cursor   = gb->gap_start; /* posición del cursor en texto visible */

    /* Calcular línea y columna del cursor */
    int cur_row = 0, cur_col = 0;
    for (size_t i = 0; i < cursor; i++) {
        if (text && text[i] == '\n') { cur_row++; cur_col = 0; }
        else cur_col++;
    }

    /* ── Barra de título ── */
    pb_append_str(pb, "\033[H");          /* ir a (1,1) */
    pb_append_str(pb, "\033[7m");         /* video inverso */

    const char *fname = gb->filepath[0] ? gb->filepath : "[Nuevo documento]";
    const char *mod   = gb->modified ? " [modificado]" : "";
    const char *algo_names[] = {"Raw", "TQLZ", "LZ77", "RLE"};
    int algo_idx = (algo >= 0 && algo <= 3) ? algo : 0;

    int hlen = snprintf(tmp, sizeof(tmp),
        " EBSO Editor | %s%s | Algo: %s | Ctrl+S=Guardar  Ctrl+Q=Salir ",
        fname, mod, algo_names[algo_idx]);

    pb_append(pb, tmp, (size_t)hlen);
    /* Rellenar hasta el final de la línea */
    for (int i = hlen; i < term_cols; i++) pb_append_str(pb, " ");
    pb_append_str(pb, "\033[0m\r\n");

    /* ── Área de texto ── */
    int display_rows = term_rows - 2; /* -2 para título y barra de estado */
    int line_num = 0;
    size_t i = 0;

    while (i <= text_len && line_num - scroll_row < display_rows) {
        if (line_num >= scroll_row) {
            /* Número de línea */
            snprintf(tmp, sizeof(tmp), "\033[90m%4d \033[0m", line_num + 1);
            pb_append_str(pb, tmp);

            /* Contenido de la línea */
            int col = 0;
            size_t j = i;
            while (j < text_len && text[j] != '\n' && col < term_cols - 5) {
                /* Marcar cursor con video inverso */
                if (j == cursor) pb_append_str(pb, "\033[7m");
                char ch = text[j];
                if (ch == '\t') {
                    pb_append_str(pb, "    "); /* tab = 4 espacios */
                    col += 4;
                } else {
                    pb_append(pb, &ch, 1);
                    col++;
                }
                if (j == cursor) pb_append_str(pb, "\033[0m");
                j++;
            }
            /* Cursor al final de la línea */
            if (j == cursor) pb_append_str(pb, "\033[7m \033[0m");
        }

        /* Saltar al inicio de la siguiente línea */
        while (i < text_len && text[i] != '\n') i++;
        if (i < text_len) i++; /* saltar el '\n' */

        if (line_num >= scroll_row) pb_append_str(pb, "\033[K\r\n");
        line_num++;

        /* Si llegamos al fin del texto sin '\n' final */
        if (i == text_len && (text_len == 0 || (text_len > 0 && text[text_len-1] != '\n'))) {
            if (cursor == text_len && line_num >= scroll_row &&
                line_num - scroll_row < display_rows)
                pb_append_str(pb, "\033[7m \033[0m\033[K\r\n");
            line_num++;
            break;
        }
    }

    /* Limpiar líneas vacías restantes */
    for (int r = line_num - scroll_row; r < display_rows; r++) {
        pb_append_str(pb, "\033[K\r\n");
    }

    /* ── Barra de estado ── */
    pb_append_str(pb, "\033[7m");
    int slen = snprintf(tmp, sizeof(tmp),
        " Línea %d | Col %d | %zu bytes | F1=Ayuda ",
        cur_row + 1, cur_col + 1, text_len);
    pb_append(pb, tmp, (size_t)slen);
    for (int s = slen; s < term_cols; s++) pb_append_str(pb, " ");
    pb_append_str(pb, "\033[0m");

    /* Posicionar cursor real del terminal */
    int disp_row = cur_row - scroll_row + 2; /* +2 por la barra de título */
    int disp_col = cur_col + 6;              /* +6 por los números de línea */
    if (disp_row < 2) disp_row = 2;
    if (disp_col < 6) disp_col = 6;
    snprintf(tmp, sizeof(tmp), "\033[%d;%dH", disp_row, disp_col);
    pb_append_str(pb, tmp);

    pb_append_str(pb, "\033[?25h"); /* Mostrar cursor */
    pb_flush(pb);

    if (text) free(text);
}

/**
 * gb_interactive_edit — Bucle principal del editor.
 *
 * Pone el terminal en modo raw, procesa teclas y redibuja la pantalla.
 * El diseño de PaintBuf reduce los write() syscalls al mínimo,
 * lo que se refleja en la salida de `strace -c ./backup_EAFITos`.
 */
int gb_interactive_edit(GapBuffer *gb, int algo) {
    if (!gb) return -1;

    if (editor_enable_raw() != 0) {
        fprintf(stderr, "Error: No se pudo activar el modo raw del terminal.\n");
        return -1;
    }

    PaintBuf pb;
    if (pb_init(&pb) != 0) {
        editor_restore_terminal();
        return -1;
    }

    int term_rows, term_cols;
    editor_get_window_size(&term_rows, &term_cols);
    int scroll_row = 0;

    /* Limpiar pantalla al entrar */
    write(STDOUT_FILENO, "\033[2J\033[H", 7);

    while (1) {
        /* Calcular fila actual del cursor */
        int cur_row = 0;
        for (size_t i = 0; i < gb->gap_start; i++) {
            if (i < gb->buf_size &&
                (i >= gb->gap_end || i < gb->gap_start)) {
                /* Contar newlines en texto pre-gap */
            }
        }
        /* Recalcular con gb_to_string para simplificar */
        char *txt = gb_to_string(gb);
        cur_row = 0;
        if (txt) {
            for (size_t i = 0; i < gb->gap_start; i++) {
                if (txt[i] == '\n') cur_row++;
            }
            free(txt);
        }

        /* Ajustar scroll */
        int display_rows = term_rows - 2;
        if (cur_row < scroll_row) scroll_row = cur_row;
        if (cur_row >= scroll_row + display_rows)
            scroll_row = cur_row - display_rows + 1;
        if (scroll_row < 0) scroll_row = 0;

        editor_render(gb, algo, scroll_row, term_rows, term_cols, &pb);

        int key = editor_read_key();
        if (key < 0) break;

        switch (key) {
            case EDITOR_CTRL('q'):  /* Salir */
                if (gb->modified) {
                    /* Pedir confirmación si hay cambios sin guardar */
                    write(STDOUT_FILENO,
                        "\033[2J\033[H\033[7m Hay cambios sin guardar. "
                        "Presiona Ctrl+Q de nuevo para salir sin guardar. \033[0m",
                        80);
                    int confirm = editor_read_key();
                    if (confirm == EDITOR_CTRL('q')) goto exit_editor;
                } else {
                    goto exit_editor;
                }
                break;

            case EDITOR_CTRL('s'):  /* Guardar */
                if (gb->filepath[0] == '\0') {
                    strncpy(gb->filepath, "Archivos/documento.ebso", MAX_PATH_EDITOR - 1);
                }
                gb_save(gb, gb->filepath, algo);
                break;

            case KEY_RIGHT:
                gb_move_cursor(gb, 1);
                break;
            case KEY_LEFT:
                gb_move_cursor(gb, -1);
                break;

            case KEY_UP: {
                /* Subir una línea: buscar inicio de línea actual y anterior */
                char *t = gb_to_string(gb);
                if (t) {
                    size_t pos = gb->gap_start;
                    /* Buscar inicio de línea actual */
                    size_t line_start = 0;
                    for (size_t j = 0; j < pos; j++)
                        if (t[j] == '\n') line_start = j + 1;
                    int col = (int)(pos - line_start);
                    if (line_start > 0) {
                        /* Buscar inicio de línea anterior */
                        size_t prev_line_start = 0;
                        for (size_t j = 0; j + 1 < line_start; j++)
                            if (t[j] == '\n') prev_line_start = j + 1;
                        size_t prev_line_len = line_start - 1 - prev_line_start;
                        size_t new_col = (size_t)col < prev_line_len ?
                                         (size_t)col : prev_line_len;
                        gb_move_gap_to(gb, prev_line_start + new_col);
                    }
                    free(t);
                }
                break;
            }

            case KEY_DOWN: {
                char *t = gb_to_string(gb);
                size_t tlen = gb_text_length(gb);
                if (t) {
                    size_t pos = gb->gap_start;
                    size_t line_start = 0;
                    for (size_t j = 0; j < pos; j++)
                        if (t[j] == '\n') line_start = j + 1;
                    int col = (int)(pos - line_start);
                    /* Buscar fin de línea actual */
                    size_t next_nl = pos;
                    while (next_nl < tlen && t[next_nl] != '\n') next_nl++;
                    if (next_nl < tlen) {
                        size_t next_line_start = next_nl + 1;
                        size_t next_nl2 = next_line_start;
                        while (next_nl2 < tlen && t[next_nl2] != '\n') next_nl2++;
                        size_t next_line_len = next_nl2 - next_line_start;
                        size_t new_col = (size_t)col < next_line_len ?
                                         (size_t)col : next_line_len;
                        gb_move_gap_to(gb, next_line_start + new_col);
                    }
                    free(t);
                }
                break;
            }

            case KEY_HOME: {
                /* Ir al inicio de la línea actual */
                char *t = gb_to_string(gb);
                if (t) {
                    size_t pos = gb->gap_start;
                    size_t line_start = 0;
                    for (size_t j = 0; j < pos; j++)
                        if (t[j] == '\n') line_start = j + 1;
                    gb_move_gap_to(gb, line_start);
                    free(t);
                }
                break;
            }

            case KEY_END: {
                /* Ir al final de la línea actual */
                char *t = gb_to_string(gb);
                size_t tlen = gb_text_length(gb);
                if (t) {
                    size_t pos = gb->gap_start;
                    while (pos < tlen && t[pos] != '\n') pos++;
                    gb_move_gap_to(gb, pos);
                    free(t);
                }
                break;
            }

            case KEY_PAGE_UP:
                gb_move_cursor(gb, -(display_rows));
                break;

            case KEY_PAGE_DOWN:
                gb_move_cursor(gb, display_rows);
                break;

            case 127: /* Backspace */
            case 8:
                gb_delete_before(gb);
                break;

            case KEY_DEL:
                gb_delete_after(gb);
                break;

            case '\r':  /* Enter */
                gb_insert_char(gb, '\n');
                break;

            default:
                /* Insertar carácter imprimible */
                if (key >= 32 && key < 127) {
                    gb_insert_char(gb, (char)key);
                }
                break;
        }
    }

exit_editor:
    pb_free(&pb);
    editor_restore_terminal();
    write(STDOUT_FILENO, "\033[2J\033[H", 7); /* Limpiar pantalla al salir */
    return 0;
}
