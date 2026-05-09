/*
 * editor.h
 * ---------------------------------------------------------------------------
 * Módulo de editor de texto con Gap Buffer y formato binario empaquetado.
 *
 * Proyecto Parcial 3: Editor de Archivos con Optimización de Bus I/O
 * Sistemas Operativos - EAFIT
 * ---------------------------------------------------------------------------
 *
 * DISEÑO DE MEMORIA:
 *   El Gap Buffer divide el texto en tres regiones contiguas en RAM:
 *
 *     [texto_antes_cursor][-------gap-------][texto_después_cursor]
 *      0                gap_start          gap_end              buf_size
 *
 *   - gap_start == posición del cursor (índice en buf)
 *   - Insertar = escribir en gap y avanzar gap_start  → O(1) amortizado
 *   - Mover cursor = transferir chars entre regiones  → O(|delta|)
 *   - El gap crece dinámicamente con realloc()
 *   - Toda la memoria se gestiona con malloc/calloc/realloc/free
 *
 * FORMATO DE ARCHIVO BINARIO (.ebso):
 *   [FileHeader 24 bytes][StyleTable opcional][payload comprimido]
 *
 *   __attribute__((packed)) garantiza que el compilador NO inserte bytes
 *   de padding entre campos. Sin esto, sizeof(FileHeader) podría ser 28
 *   en lugar de 24, corrompiendo la lectura con read()/write() crudos.
 */

#ifndef EDITOR_H
#define EDITOR_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* =========================================================================
 * CONSTANTES
 * ========================================================================= */

#define EBSO_MAGIC           "EBSO"  /**< Identificador mágico del formato  */
#define EBSO_VERSION         1       /**< Versión actual del formato         */
#define GAP_INITIAL_SIZE     4096    /**< Tamaño inicial del gap (1 página)  */
#define GAP_GROW_SIZE        4096    /**< Incremento cuando el gap se agota  */
#define MAX_PATH_EDITOR      1024    /**< Longitud máxima de ruta de archivo */
#define MAX_STYLE_ENTRIES    128     /**< Máx. entradas en tabla de estilos  */

/** Flags del campo FileHeader.flags */
#define EBSO_FLAG_HAS_STYLES 0x0001  /**< El archivo incluye tabla de estilos */

/** Bitmask de estilos para StyleEntry.style */
#define STYLE_BOLD           0x01
#define STYLE_ITALIC         0x02
#define STYLE_UNDERLINE      0x04

/* =========================================================================
 * ESTRUCTURAS EMPAQUETADAS — FORMATO BINARIO .ebso
 *
 * __attribute__((packed)) instruye al compilador a colocar los campos
 * exactamente uno tras otro, sin relleno de alineación. Esto es crítico
 * para I/O binario correcto: cuando hacemos write(fd, &header, sizeof(FileHeader))
 * necesitamos saber exactamente cuántos bytes se escriben y en qué orden.
 *
 * Verificación en tiempo de compilación (C11):
 *   _Static_assert(sizeof(FileHeader) == 24, "FileHeader debe ser 24 bytes");
 * ========================================================================= */

/**
 * FileHeader — cabecera principal del formato .ebso
 *
 * Distribución exacta en memoria (24 bytes sin padding):
 *   Offset  0: magic[4]          — 4 bytes
 *   Offset  4: version           — 1 byte
 *   Offset  5: algo              — 1 byte
 *   Offset  6: flags             — 2 bytes
 *   Offset  8: original_size     — 4 bytes
 *   Offset 12: compressed_size   — 4 bytes
 *   Offset 16: timestamp         — 8 bytes
 *   Total: 24 bytes
 *
 * Sin __attribute__((packed)), el compilador alinearía original_size a
 * un múltiplo de 4 bytes, insertando 2 bytes de padding después de flags,
 * resultando en 26 bytes — lo cual rompería la lectura portátil del header.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];        /**< "EBSO" — Editor Backup Sistemas Operativos */
    uint8_t  version;         /**< Versión del formato (actualmente 1)        */
    uint8_t  algo;            /**< Algoritmo: 1=TQLZ, 2=LZ77, 3=RLE, 0=Raw   */
    uint16_t flags;           /**< Bitmask: EBSO_FLAG_HAS_STYLES, etc.        */
    uint32_t original_size;   /**< Tamaño del texto plano descomprimido        */
    uint32_t compressed_size; /**< Tamaño del payload comprimido en disco      */
    uint64_t timestamp;       /**< Unix timestamp de la última modificación    */
} FileHeader;

/**
 * StyleEntry — un tramo de texto con estilo aplicado
 *
 * Distribución exacta en memoria (10 bytes sin padding):
 *   Offset 0: offset   — 4 bytes
 *   Offset 4: length   — 4 bytes
 *   Offset 8: style    — 1 byte
 *   Offset 9: reserved — 1 byte
 *   Total: 10 bytes
 */
typedef struct __attribute__((packed)) {
    uint32_t offset;    /**< Byte de inicio en el texto plano              */
    uint32_t length;    /**< Cantidad de bytes que abarca este estilo      */
    uint8_t  style;     /**< Bitmask: STYLE_BOLD | STYLE_ITALIC | ...      */
    uint8_t  reserved;  /**< Padding explícito reservado para uso futuro   */
} StyleEntry;

/**
 * StyleTableHeader — precede al arreglo de StyleEntry en el archivo
 * Tamaño garantizado: 2 bytes.
 */
typedef struct __attribute__((packed)) {
    uint16_t count;  /**< Número de StyleEntry que siguen en el archivo */
} StyleTableHeader;

/* =========================================================================
 * ESTRUCTURA PRINCIPAL: Gap Buffer
 * ========================================================================= */

/**
 * GapBuffer — estructura del editor de texto en memoria.
 *
 * Invariantes que siempre se cumplen:
 *   0 <= gap_start <= gap_end <= buf_size
 *   buf apunta a un bloque de buf_size bytes en el heap
 *   texto visible = buf[0..gap_start) + buf[gap_end..buf_size)
 */
typedef struct {
    char   *buf;                      /**< Buffer dinámico (malloc/realloc)   */
    size_t  buf_size;                 /**< Capacidad total asignada            */
    size_t  gap_start;                /**< Inicio del gap == posición cursor   */
    size_t  gap_end;                  /**< Fin del gap (exclusivo)             */
    int     modified;                 /**< 1 si hay cambios sin guardar        */
    char    filepath[MAX_PATH_EDITOR];/**< Ruta del archivo actualmente abierto*/
} GapBuffer;

/* =========================================================================
 * API PÚBLICA
 * ========================================================================= */

/** Crear un Gap Buffer vacío. Usa calloc internamente. Retorna NULL en OOM. */
GapBuffer *gb_create(void);

/** Liberar toda la memoria dinámica del Gap Buffer. */
void       gb_destroy(GapBuffer *gb);

/** Insertar un carácter en la posición del cursor. */
int        gb_insert_char(GapBuffer *gb, char c);

/** Insertar una cadena de `len` bytes en la posición del cursor. */
int        gb_insert_string(GapBuffer *gb, const char *str, size_t len);

/** Eliminar el carácter ANTES del cursor (Backspace). */
int        gb_delete_before(GapBuffer *gb);

/** Eliminar el carácter DESPUÉS del cursor (Delete). */
int        gb_delete_after(GapBuffer *gb);

/** Mover el cursor `delta` posiciones (positivo=adelante, negativo=atrás). */
int        gb_move_cursor(GapBuffer *gb, int delta);

/** Mover el cursor al inicio absoluto del documento. */
void       gb_cursor_home(GapBuffer *gb);

/** Mover el cursor al final absoluto del documento. */
void       gb_cursor_end(GapBuffer *gb);

/** Retorna el número de bytes de texto visible (sin contar el gap). */
size_t     gb_text_length(const GapBuffer *gb);

/**
 * Convertir el Gap Buffer a una cadena C contigua.
 * El llamador DEBE liberar el puntero retornado con free().
 * Retorna NULL en caso de error de memoria.
 */
char      *gb_to_string(const GapBuffer *gb);

/**
 * Guardar el documento en formato binario .ebso.
 * Pipeline: texto → compresión en RAM → write() al disco.
 * Ningún byte de texto plano viaja al disco.
 * Usa POSIX open()/write() — sin stdio.h.
 *
 * @param algo  Algoritmo: ALG_TURBOQUANT_LZ(1), ALG_LZ77(2), ALG_RLE(3)
 */
int        gb_save(GapBuffer *gb, const char *path, int algo);

/**
 * Cargar un documento .ebso desde disco.
 * Lee el FileHeader, verifica el magic, descomprime el payload.
 * Usa POSIX open()/read() — sin stdio.h.
 */
int        gb_load(GapBuffer *gb, const char *path);

/**
 * Cargar un archivo de texto plano usando mmap.
 *
 * En lugar de leer el archivo con read() en un bucle, mmap() mapea el
 * archivo directamente al espacio de direcciones del proceso. El kernel
 * maneja las page faults bajo demanda, eliminando la copia intermedia
 * del buffer del kernel al buffer de usuario (zero-copy desde caché).
 *
 * Útil para comparar: read() con buffer 4KB  vs  mmap() sin buffer.
 */
int        gb_load_plaintext_mmap(GapBuffer *gb, const char *path);

/** Imprimir el contenido del documento a stdout usando write(). */
void       gb_print(const GapBuffer *gb);

/**
 * Lanzar el editor interactivo en modo raw de terminal.
 *
 * Controles:
 *   Flechas      — mover cursor
 *   Ctrl+S       — guardar
 *   Ctrl+Q       — salir
 *   Backspace    — borrar hacia atrás
 *   Delete       — borrar hacia adelante
 *   Home/End     — inicio/fin de línea
 *   Ctrl+Home    — inicio del documento
 *   Ctrl+End     — fin del documento
 */
int        gb_interactive_edit(GapBuffer *gb, int algo);

#endif /* EDITOR_H */
