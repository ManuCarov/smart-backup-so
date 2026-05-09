/*
 * smart_copy.h
 * ---------------------------------------------------------------------------
 * Definición de constantes, flags y firmas del motor de backup "kernel-space".
 *
 * Proyecto Parcial: Smart Backup Kernel-Space Utility
 * Sistemas Operativos - EAFIT
 * ---------------------------------------------------------------------------
 */

#ifndef SMART_COPY_H
#define SMART_COPY_H

#include <sys/types.h>   /* off_t, ssize_t */

/* =========================================================================
 * CONSTANTES GLOBALES
 * ========================================================================= */

/** Tamaño del buffer de página (4 KB) — maximiza el throughput de I/O */
#define BUFFER_SIZE       4096

/** Ruta del archivo de log del sistema (simula registro de kernel) */
#define LOG_FILE          "Archivos/smart_backup.log"

/** Longitud máxima de una ruta de archivo */
#define MAX_PATH_LEN      1024

/* =========================================================================
 * CÓDIGOS DE RETORNO
 * ========================================================================= */

#define SC_OK             0   /**< Operación exitosa                        */
#define SC_ERR_OPEN      -1   /**< No se pudo abrir el archivo origen       */
#define SC_ERR_CREATE    -2   /**< No se pudo crear el archivo destino      */
#define SC_ERR_READ      -3   /**< Error durante la lectura                 */
#define SC_ERR_WRITE     -4   /**< Error durante la escritura (disco lleno) */
#define SC_ERR_STAT      -5   /**< No se pudo obtener info del archivo      */
#define SC_ERR_PERM      -6   /**< Permiso denegado                         */
#define SC_ERR_NULLPTR   -7   /**< Puntero nulo recibido como argumento     */
#define SC_ERR_MKDIR     -8   /**< No se pudo crear directorio destino      */
#define SC_ERR_OPENDIR   -9   /**< No se pudo abrir directorio origen       */

/* =========================================================================
 * FLAGS DE COMPORTAMIENTO
 * ========================================================================= */

/** Sin flags especiales */
#define SCOPY_NONE         0x00

/** Sobreescribir el archivo destino si ya existe */
#define SCOPY_OVERWRITE    0x01

/** Preservar permisos del archivo original en el destino */
#define SCOPY_PRESERVE     0x02

/** Modo verbose: imprimir cada operación en stdout */
#define SCOPY_VERBOSE      0x04

/** Registrar operaciones en el archivo de log (LOG_FILE) */
#define SCOPY_LOG          0x08

/** Activar compresión durante la copia */
#define SCOPY_COMPRESS     0x10

/** Activar restauración/descompresión durante la copia */
#define SCOPY_RESTORE      0x20

/** Activar encriptaciÃ³n/desencriptaciÃ³n XOR simÃ©trica */
#define SCOPY_ENCRYPT      0x40

#define ALG_TURBOQUANT_LZ  1
#define ALG_LZ77           2
#define ALG_RLE            3

/* =========================================================================
 * ESTRUCTURA DE ESTADÍSTICAS DE COPIA
 * ========================================================================= */

/**
 * CopyStats — acumula métricas de una operación de respaldo.
 * Se llena durante la ejecución de sys_smart_copy / sys_smart_copy_dir.
 */
typedef struct {
    long   files_copied;    /**< Número de archivos copiados con éxito    */
    long   files_failed;    /**< Número de archivos que fallaron           */
    long   dirs_created;    /**< Número de directorios creados             */
    off_t  bytes_copied;    /**< Total de bytes copiados                   */
    off_t  original_bytes;  /**< Total de bytes leídos (originales)        */
} CopyStats;

/* =========================================================================
 * FIRMAS DE FUNCIONES PÚBLICAS
 * ========================================================================= */

/**
 * sys_smart_copy — copia un archivo individual usando syscalls de bajo nivel.
 *
 * Abre origen con O_RDONLY, crea/sobreescribe destino con O_WRONLY | O_CREAT,
 * transfiere datos en bloques de BUFFER_SIZE bytes y cierra ambos descriptores.
 * Valida permisos, punteros y maneja errno en cada paso.
 *
 * @param src    Ruta del archivo origen (no puede ser NULL).
 * @param dest   Ruta del archivo destino (no puede ser NULL).
 * @param flags  Combinación de flags SCOPY_*.
 * @param algo   Algoritmo de compresión a usar (ALG_*).
 * @param stats  Puntero a CopyStats donde se acumulan métricas (puede ser NULL).
 * @return       SC_OK en éxito, o un código SC_ERR_* en fallo.
 */
int sys_smart_copy(const char *src, const char *dest,
                   int flags, int algo, CopyStats *stats);

/**
 * sys_smart_copy_dir — copia recursivamente un directorio completo.
 *
 * Recorre el árbol de directorios con opendir/readdir, crea la estructura
 * espejo en destino y delega cada archivo a sys_smart_copy.
 *
 * @param src    Ruta del directorio origen.
 * @param dest   Ruta del directorio destino.
 * @param flags  Combinación de flags SCOPY_*.
 * @param algo   Algoritmo de compresión a usar (ALG_*).
 * @param stats  Puntero a CopyStats (puede ser NULL).
 * @return       SC_OK si todos los archivos se copiaron, SC_ERR_* en fallo.
 */
int sys_smart_copy_dir(const char *src, const char *dest,
                       int flags, int algo, CopyStats *stats);

/**
 * log_operation — registra un mensaje en LOG_FILE con timestamp.
 *
 * Simula el mecanismo de logging del kernel (printk / dmesg).
 * Abre el log en modo append para no perder entradas anteriores.
 *
 * @param level   Nivel del mensaje (ej: "INFO", "ERROR", "WARN").
 * @param message Texto a registrar.
 */
void log_operation(const char *level, const char *message);

/**
 * print_stats — imprime en stdout un resumen de las estadísticas de copia.
 *
 * @param stats  Puntero a la estructura CopyStats a mostrar.
 */
void print_stats(const CopyStats *stats);

/* =========================================================================
 * FUNCIONES DE COMPRESIÓN EN MEMORIA (definidas en backup_engine.c)
 *
 * Operan enteramente sobre buffers en RAM — ningún byte plano va al disco.
 * El llamador asigna out_data con malloc(in_size * 2) para el peor caso.
 * ========================================================================= */

/** Comprimir con RLE. Retorna bytes escritos en out_data. */
size_t compress_rle_buffer(const char *in_data, size_t in_size, char *out_data);

/** Descomprimir con RLE. Retorna bytes escritos en out_data. */
size_t decompress_rle_buffer(const char *in_data, size_t in_size, char *out_data);

/** Comprimir con LZ77. Retorna bytes escritos en out_data. */
size_t compress_lz77_buffer(const char *in_data, size_t in_size, char *out_data);

/** Descomprimir con LZ77. Retorna bytes escritos en out_data. */
size_t decompress_lz77_buffer(const char *in_data, size_t in_size, char *out_data);

/* =========================================================================
 * COPIA CON mmap (definida en backup_engine.c)
 *
 * Alternativa a sys_smart_copy que usa mmap() en lugar de read()/write().
 * Mapea origen y destino en memoria virtual; el kernel gestiona el I/O
 * mediante page faults, reduciendo las llamadas al sistema explícitas.
 * Útil para comparar empíricamente con strace cuántos context switches
 * genera cada enfoque.
 * ========================================================================= */

/**
 * sys_mmap_copy — copia un archivo usando mmap() en lugar de read()/write().
 *
 * @param src    Ruta del archivo origen.
 * @param dest   Ruta del archivo destino.
 * @param stats  Puntero a CopyStats (puede ser NULL).
 * @return       SC_OK en éxito, SC_ERR_* en fallo.
 */
int sys_mmap_copy(const char *src, const char *dest, CopyStats *stats);

#endif /* SMART_COPY_H */
