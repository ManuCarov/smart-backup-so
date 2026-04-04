#include <stdio.h>
#include "smart_copy.h"

int main() {
    CopyStats stats = {0, 0, 0, 0};
    int flags = SCOPY_OVERWRITE | SCOPY_VERBOSE | SCOPY_LOG;

    printf("=== Prueba copia de archivo ===\n");
    int ret = sys_smart_copy("origen.txt", "destino.txt", flags, &stats);
    printf("Resultado: %s\n\n", ret == 0 ? "EXITOSO" : "FALLO");

    print_stats(&stats);
    return 0;
}
