#include <stdio.h>
#include <stdlib.h>

int main() {
    int i;
    int* data = malloc(1024 * sizeof(int));
    
    for (i = 0; i < 1024; i++) {
        data[i] = i;
    }
    
    for (i = 0; i < 1024; i++) {
        printf("%d ", data[i]);
    }
    
    free(data);
    return 0;
}
