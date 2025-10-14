#include <stdio.h>
#include <stdlib.h>

int main() {
    int i, j, k;
    int size = 64;
    int** a = malloc(size * sizeof(int*));
    int** b = malloc(size * sizeof(int*));
    int** c = malloc(size * sizeof(int*));
    
    for (i = 0; i < size; i++) {
        a[i] = malloc(size * sizeof(int));
        b[i] = malloc(size * sizeof(int));
        c[i] = malloc(size * sizeof(int));
    }
    
    for (i = 0; i < size; i++) {
        for (j = 0; j < size; j++) {
            c[i][j] = 0;
            for (k = 0; k < size; k++) {
                c[i][j] = c[i][j] + a[i][k] * b[k][j];
            }
        }
    }
    
    for (i = 0; i < size; i++) {
        free(a[i]);
        free(b[i]);
        free(c[i]);
    }
    free(a);
    free(b);
    free(c);
    
    return 0;
}
