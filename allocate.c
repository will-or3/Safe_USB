// gcc -Wall -Wextra -o fake_usb allocate.c
// ./fake_usb create fake_usb.img 5G
// ./fake_usb info fake_usb.img

#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// force 1 byte
typedef struct __attribute__((packed)) {
    char magic[8];      // 8 bytes
    uint32_t version;   // 4 bytes
    uint32_t data_offset; // 4 bytes
} Header; // 16 bytes

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

int write_header(int fd) {
    Header header = {
        .magic = "SAFEUSB",
        .version = 1,
        .data_offset = 4096
    };

    // add err handling later

    // set to first (0) btye 
    lseek(fd, 0, SEEK_SET);

    write(fd, &header, sizeof(header));
    fsync(fd);

    return 0;
}

int read_header(int fd, Header *header) {
    lseek(fd, 0, SEEK_SET);

    read(fd, header, sizeof(*header));

    if (memcmp(header->magic, "SAFEUSB", 7) != 0) {
        return -1; // not our drive
    }

    return 0;
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

    write_header(fd);

    close(fd);

    printf("Created fake_usb.img (%lld bytes)\n", (long long)size);

    return 0;
}

void print_header(const Header *header){
    printf("magic: %.8s\n", header->magic);
    printf("version: %u\n", header->version);
    printf("data offset: %u\n", header->data_offset);
}

int info(const char *file_name) {
    int fd = open(file_name, O_RDONLY);
        if (fd == -1) {
            perror("cant open");
            return 1;
        }

        Header header;
        
        if (read_header(fd, &header) != 0) {
            printf("not a SAFEUSB image.\n");
            close(fd);
            return 1;
        }

        print_header(&header);

        close(fd);

        return 0;
}

int main(int argc, char *argv[]){
    if (argc < 2) {
        goto usage; }
    else if (strcmp(argv[1], "create") == 0) {
        if (argc != 4)
            goto usage;
    }
    else if (strcmp(argv[1], "info") == 0) {
        if (argc != 3)
            goto usage;
    }
    else {
        goto usage;
    }

    const char *file_name = argv[2];

    if (strcmp(argv[1], "create") == 0) {
        off_t size = parse_size(argv[3]);
        return allocate(file_name, size);
    } else if (strcmp(argv[1], "info") == 0) {
        return info(file_name);
    } else {
        goto usage;
    }

    usage:
        fprintf(stderr, "Usage: %s create <filename> <size[K|M|G]>\n", argv[0]);
        fprintf(stderr, "Usage: %s info <filename>\n", argv[0]);
        return 1;
}