// gcc -Wall -Wextra -o fake_usb allocate.c
// ./fake_usb create fake_usb.img 5G

#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

off_t parse_size(const char *size_str) {
    char *endptr;
    long long value = strtoll(size_str, &endptr, 10);

    if (endptr == size_str || value <= 0) {
        return 0; 
    }

    switch (*endptr) {
        case 'G': case 'g':
            return (off_t)value * 1024 * 1024 * 1024;
        case 'M': case 'm':
            return (off_t)value * 1024 * 1024;
        case 'K': case 'k':
            return (off_t)value * 1024;
        case '\0':
            return (off_t)value;
        default:
            return 0;
    }
}



int allocate(const char *file_name, off_t size){
    int fd = open(
        file_name, 
        O_RDWR | O_CREAT | O_TRUNC, 
        0644);

    if (fd == -1) {
        perror("couldnt open");
        return 1;
    }
    
    if (posix_fallocate(fd, 0, size) != 0) {
        perror("posix_fallocate");
        close(fd);
        return 1;
    }

    close(fd);

    printf("Created fake_usb.img (%lld bytes)\n", (long long)size);

    return 0;
}


int main(int argc, char *argv[]){
    if (argc != 4 || strcmp(argv[1], "create") != 0) {
        fprintf(stderr, "Usage: %s create <filename> <size[K|M|G]>\n", argv[0]);
        return 1;
    }

    const char *file_name = argv[2];
    off_t size = parse_size(argv[3]);

    return allocate(file_name, size);
}