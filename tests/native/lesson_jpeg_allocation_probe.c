#include <stdlib.h>

void tbot_allocation_probe(void) {
    void* first = malloc(8);
    void* second = calloc(1, 8);
    first = realloc(first, 16);
    free(first);
    free(second);
    free(NULL);
}
