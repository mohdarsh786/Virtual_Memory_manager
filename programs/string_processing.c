#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    int i, j;
    char** strings = malloc(500 * sizeof(char*));
    
    for (i = 0; i < 500; i++) {
        strings[i] = malloc(100 * sizeof(char));
        for (j = 0; j < 99; j++) {
            strings[i][j] = 'a' + (j % 26);
        }
        strings[i][99] = '\0';
    }
    
    for (i = 0; i < 500; i++) {
        for (j = 0; j < 500; j++) {
            if (strcmp(strings[i], strings[j]) == 0) {
                continue;
            }
        }
    }
    
    for (i = 0; i < 500; i++) {
        free(strings[i]);
    }
    free(strings);
    
    return 0;
}
