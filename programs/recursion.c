#include <stdio.h>
#include <stdlib.h>

int fibonacci(int n) {
    int* temp = malloc(sizeof(int));
    *temp = n;
    
    if (n <= 1) {
        free(temp);
        return n;
    }
    
    int result = fibonacci(n - 1) + fibonacci(n - 2);
    free(temp);
    return result;
}

int main() {
    int i;
    int* results = malloc(20 * sizeof(int));
    
    for (i = 0; i < 20; i++) {
        results[i] = fibonacci(i);
    }
    
    free(results);
    return 0;
}
