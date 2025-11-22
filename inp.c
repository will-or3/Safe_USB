#include <stdio.h>
#include <string.h>

int main() {
    char buffer_1[100];
    char buffer_2[100];
    printf("Enter pwd >:");
    fgets(buffer_1, sizeof(buffer_1), stdin);
    
    printf("Enter pwd again >:");
    fgets(buffer_2, sizeof(buffer_2), stdin);
    
    if (strcmp(buffer_1, buffer_2) == 0) {
        printf("there the same");
    } else {
        printf("not the same");
    }
}