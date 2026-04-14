#include <stdio.h>
#include <time.h>

int main() {
    time_t start = time(NULL);
    volatile unsigned long long x = 0;

    while (time(NULL) - start < 60) {
        x = x * 1664525ULL + 1013904223ULL;
    }

    printf("done %llu\n", x);
    return 0;
}
