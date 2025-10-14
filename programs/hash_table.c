#include <stdio.h>
#include <stdlib.h>

struct entry {
    int key;
    int value;
    struct entry* next;
};

int main() {
    int i, idx;
    int table_size = 256;
    struct entry** hash_table = malloc(table_size * sizeof(struct entry*));
    
    for (i = 0; i < table_size; i++) {
        hash_table[i] = NULL;
    }
    
    for (i = 0; i < 2000; i++) {
        idx = i % table_size;
        struct entry* e = malloc(sizeof(struct entry));
        e->key = i;
        e->value = i * i;
        e->next = hash_table[idx];
        hash_table[idx] = e;
    }
    
    for (i = 0; i < table_size; i++) {
        struct entry* current = hash_table[i];
        while (current != NULL) {
            struct entry* temp = current;
            current = current->next;
            free(temp);
        }
    }
    
    free(hash_table);
    return 0;
}
