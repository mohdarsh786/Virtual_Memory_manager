#include <stdio.h>
#include <stdlib.h>

int main() {
    int i, idx;
    int* data = malloc(2048 * sizeof(int));
    
    for (i = 0; i < 2048; i++) {
        data[i] = 0;
    }
    
    for (i = 0; i < 5000; i++) {
        idx = rand() % 2048;
        data[idx] = data[idx] + 1;
    }
    
    free(data);
    return 0;
}
