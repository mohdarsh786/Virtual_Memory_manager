#include <stdio.h>
#include <stdlib.h>

int main() {
    int i, low, high, mid, target;
    int size = 1800;
    int* arr = malloc(size * sizeof(int));
    
    for (i = 0; i < size; i++) {
        arr[i] = i * 2;
    }
    
    for (target = 0; target < 1000; target += 10) {
        low = 0;
        high = size - 1;
        
        while (low <= high) {
            mid = (low + high) / 2;
            
            if (arr[mid] == target) {
                break;
            } else if (arr[mid] < target) {
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
    }
    
    free(arr);
    return 0;
}
