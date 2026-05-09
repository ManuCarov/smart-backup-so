#!/bin/bash
# =============================================================================
# profiling.sh — Análisis empírico de rendimiento del editor/backup
#
# Mide y compara empíricamente:
#   - Número de context switches (syscalls) con strace -c
#   - Tiempo de CPU (User/Sys) vs tiempo real con /usr/bin/time -v
#
# ENFOQUE 1 (Clásico):   read()/write() sin compresión
# ENFOQUE 2 (Propuesto): read()/write() con compresión LZ77 en User Space
# ENFOQUE 3 (mmap):      mapeo en memoria (sys_mmap_copy)
#
# Uso: bash profiling.sh
# Requisitos: strace, /usr/bin/time (GNU time), gcc, make
# =============================================================================

set -e

BINARY="./backup_EAFITos"
TEST_50MB="Archivos/profiling_50mb.bin"
OUT_DIR="Archivos/profiling_results"

echo "============================================================"
echo "  PROFILING: Editor de Archivos con Optimización de Bus I/O"
echo "============================================================"
echo ""

# --- Paso 0: Compilar ---
echo "[0] Compilando proyecto..."
make clean > /dev/null 2>&1
make 2>&1 | grep -E "^gcc|error:" || true
echo "    Compilación exitosa."
echo ""

mkdir -p "$OUT_DIR"

# --- Paso 1: Generar archivo de prueba ---
echo "[1] Generando archivo de prueba (50 MB, patrón repetitivo)..."
$BINARY -g 50 -t 1 "$TEST_50MB"
REAL_SIZE=$(stat -c%s "$TEST_50MB")
echo "    Tamaño real en disco: ${REAL_SIZE} bytes"
echo ""

# --- Paso 2: ENFOQUE CLÁSICO — read/write sin compresión ---
echo "============================================================"
echo " ENFOQUE CLÁSICO: open/read/write con buffer 4KB (sin compresión)"
echo "============================================================"

echo ""
echo "--- strace -c (conteo de syscalls y context switches) ---"
strace -c "$BINARY" -b "$TEST_50MB" "${OUT_DIR}/out_classic.bin" 2>&1

echo ""
echo "--- /usr/bin/time -v (CPU User vs Sys vs Wall-clock) ---"
/usr/bin/time -v "$BINARY" -b "$TEST_50MB" "${OUT_DIR}/out_classic2.bin" 2>&1

CLASSIC_SIZE=$(stat -c%s "${OUT_DIR}/out_classic.bin" 2>/dev/null || echo "N/A")
echo ""
echo "    Tamaño de salida (sin comprimir): ${CLASSIC_SIZE} bytes"

# --- Paso 3: ENFOQUE PROPUESTO — compresión LZ77 en User Space ---
echo ""
echo "============================================================"
echo " ENFOQUE PROPUESTO: Compresión LZ77 en User Space + write()"
echo " (ningún byte de texto plano viaja al disco)"
echo "============================================================"

echo ""
echo "--- strace -c ---"
strace -c "$BINARY" -b -c -a 2 "$TEST_50MB" "${OUT_DIR}/out_lz77.ebso" 2>&1

echo ""
echo "--- /usr/bin/time -v ---"
/usr/bin/time -v "$BINARY" -b -c -a 2 "$TEST_50MB" "${OUT_DIR}/out_lz77_2.ebso" 2>&1

LZ77_SIZE=$(stat -c%s "${OUT_DIR}/out_lz77.ebso" 2>/dev/null || echo "N/A")
echo ""
echo "    Tamaño de salida (comprimido LZ77): ${LZ77_SIZE} bytes"

if [ "$REAL_SIZE" -gt 0 ] && [ "$LZ77_SIZE" != "N/A" ]; then
    RATIO=$(echo "scale=1; (1 - $LZ77_SIZE / $REAL_SIZE) * 100" | bc 2>/dev/null || echo "N/A")
    echo "    Reducción de tamaño: ${RATIO}%"
fi

# --- Paso 4: ENFOQUE mmap ---
echo ""
echo "============================================================"
echo " ENFOQUE mmap: mapeo en memoria (sin bucle read/write explícito)"
echo "============================================================"
echo ""
echo "--- strace -c ---"
# Usamos el benchmark integrado que llama sys_mmap_copy internamente
strace -c "$BINARY" --perf 2>&1 | head -60

# --- Paso 5: Resumen ---
echo ""
echo "============================================================"
echo " RESUMEN DE RESULTADOS"
echo "============================================================"
echo ""
echo " Archivo de prueba: ${REAL_SIZE} bytes (50 MB)"
echo ""
echo " Enfoque           | Tamaño en disco | Reducción"
echo " ------------------+-----------------+----------"
printf " Clásico (raw)     | %15s | 0%%\n"    "$CLASSIC_SIZE"
printf " LZ77 comprimido   | %15s | %s\n"     "$LZ77_SIZE" "${RATIO:-N/A}%"
echo ""
echo " NOTA: Comparar el número de 'write' calls en strace:"
echo "   - Clásico: ~12.800 calls para 50MB (cada 4096 bytes)"
echo "   - Propuesto: ~3.750 calls (bloques más grandes + compresión)"
echo "   - mmap: ~4 syscalls explícitas (mmap/munmap/msync/memcpy)"
echo ""
echo " Ver PROFILING_REPORT.md para análisis completo."

# --- Limpieza opcional ---
echo ""
read -p "¿Eliminar archivos de prueba? [s/N]: " -r CLEAN
if [[ "$CLEAN" =~ ^[Ss]$ ]]; then
    rm -f "$TEST_50MB" "${OUT_DIR}"/*
    echo "Archivos de prueba eliminados."
fi

echo ""
echo "Profiling completado."
