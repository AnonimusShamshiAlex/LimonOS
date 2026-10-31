#include <stdlib.h>
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
#include <stdio.h>
#include "ai.h"
static void on_line(const char *s, void *ctx) { (void)ctx; printf("%s\n", s); }
int main(int argc, char **argv) {
    ai_set_key(argv[1]);
    ai_dump_request(argv[2], on_line, NULL);
    return 0;
}
