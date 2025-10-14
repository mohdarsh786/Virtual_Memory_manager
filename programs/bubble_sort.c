#include <stdio.h>
#include <stdlib.h>

int main() {
    int i, j, temp;
    int size = 1500;
    int* arr = malloc(size * sizeof(int));
    
    for (i = 0; i < size; i++) {
        arr[i] = size - i;
    }
    
    for (i = 0; i < size - 1; i++) {
        for (j = 0; j < size - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
    
    free(arr);
    return 0;
}
