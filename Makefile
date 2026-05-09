CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=c11
TARGET  = backup_EAFITos
SRC     = backup.c backup_engine.c editor.c

# Regla por defecto
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

# Limpiar binarios y archivos temporales
clean:
	rm -f $(TARGET)
	rm -rf src_test dest_test test_file.txt test_file_backup.txt Archivos/

# Pruebas automáticas básicas
test: $(TARGET)
	@echo "\n--- [1] PREPARANDO ENTORNO ---"
	mkdir -p src_test
	echo "Hola, este es un archivo de prueba 1" > src_test/archivo1.txt
	echo "Esto es otra prueba con más contenido para comprimir" > src_test/archivo2.txt
	mkdir -p src_test/subcarpeta
	echo "Archivo en subcarpeta" > src_test/subcarpeta/archivo3.txt
	echo "Archivo simple para backup" > test_file.txt

	@echo "\n--- [2] BACKUP DE ARCHIVO SIMPLE ---"
	./$(TARGET) -b test_file.txt test_file_backup.txt

	@echo "\n--- [3] BACKUP CON COMPRESIÓN RLE ---"
	./$(TARGET) -b -c -a 3 test_file.txt Archivos/test_rle.ebso

	@echo "\n--- [4] RESTAURAR ARCHIVO COMPRIMIDO ---"
	./$(TARGET) -b -r -a 3 Archivos/test_rle.ebso Archivos/test_restored.txt

	@echo "\n--- [5] BACKUP DE DIRECTORIO ---"
	./$(TARGET) -b src_test dest_test

	@echo "\n--- [6] RESULTADO DEL DIRECTORIO DE BACKUP ---"
	ls -R dest_test

# Verificar memoria con valgrind
valgrind-check: $(TARGET)
	@echo "Verificando fugas de memoria con valgrind..."
	mkdir -p Archivos
	echo "Texto de prueba para valgrind" > /tmp/valgrind_test.txt
	valgrind --leak-check=full --error-exitcode=1 \
	  ./$(TARGET) -b /tmp/valgrind_test.txt Archivos/valgrind_out.txt
	@echo "Valgrind: sin fugas de memoria detectadas"

# Benchmark de rendimiento (read/write vs mmap vs stdio)
perf: $(TARGET)
	./$(TARGET) --perf

# Profiling con strace (requiere strace instalado)
profiling: $(TARGET)
	@echo "Ejecutando profiling con strace..."
	mkdir -p Archivos
	./$(TARGET) -g 10 Archivos/profiling_test.bin
	@echo "\n=== strace: sys_smart_copy (read/write 4KB) ==="
	strace -c ./$(TARGET) -b Archivos/profiling_test.bin Archivos/prof_out_syscall.bin 2>&1
	@echo "\n=== time: sys_smart_copy ==="
	/usr/bin/time -v ./$(TARGET) -b Archivos/profiling_test.bin Archivos/prof_out_syscall2.bin 2>&1
	@echo "\nVer PROFILING_REPORT.md para análisis completo"

.PHONY: all clean test valgrind-check perf profiling

