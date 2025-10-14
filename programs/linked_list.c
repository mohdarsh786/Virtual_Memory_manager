#include <stdio.h>
#include <stdlib.h>

struct node {
    int data;
    struct node* next;
};

int main() {
    int i;
    struct node* head = NULL;
    struct node* current = NULL;
    
    for (i = 0; i < 1000; i++) {
        struct node* new_node = malloc(sizeof(struct node));
        new_node->data = i;
        new_node->next = NULL;
        
        if (head == NULL) {
            head = new_node;
            current = head;
        } else {
            current->next = new_node;
            current = new_node;
        }
    }
    
    current = head;
    while (current != NULL) {
        struct node* temp = current;
        current = current->next;
        free(temp);
    }
    
    return 0;
}
