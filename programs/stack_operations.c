#include <stdio.h>
#include <stdlib.h>

struct stack {
    int* data;
    int top;
    int capacity;
};

int main() {
    int i;
    struct stack s;
    s.capacity = 2000;
    s.top = -1;
    s.data = malloc(s.capacity * sizeof(int));
    
    for (i = 0; i < 1500; i++) {
        s.top = s.top + 1;
        s.data[s.top] = i;
    }
    
    for (i = 0; i < 1000; i++) {
        if (s.top >= 0) {
            s.top = s.top - 1;
        }
    }
    
    for (i = 0; i < 800; i++) {
        s.top = s.top + 1;
        s.data[s.top] = i * 2;
    }
    
    free(s.data);
    return 0;
}
