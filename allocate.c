// gcc -Wall -Wextra -o fake_usb allocate.c
// ./fake_usb create fake_usb.img 5G (for no hidden)
// ./fake_usb create fake_usb.img 5G --hidden 2G
// ./fake_usb info fake_usb.img

#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>


typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;

    uint64_t normal_offset;
    uint64_t normal_size;

    uint64_t hidden_offset;
    uint64_t hidden_size;

} Header;

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

int write_header(int fd, Header *header) {
    
    // add err handling later

    // set to first (0) btye 
    lseek(fd, 0, SEEK_SET);

    write(fd, header, sizeof(header));
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

int allocate(const char *file_name, off_t size, off_t hidden_size){
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

    Header header = {0};

    memcpy(header.magic, "SAFEUSB", 7);
    header.version = 1;

    header.normal_offset = sizeof(Header);

    if (hidden_size > 0) {

        header.hidden_size = hidden_size;

        header.normal_size = size - hidden_size;

        header.hidden_offset =
            header.normal_offset + header.normal_size;

    } else {

        header.normal_size = size;

        header.hidden_size = 0;
        header.hidden_offset = 0;

    }

    write_header(fd, &header);

    close(fd);

    printf("Created fake_usb.img (%lld bytes)\n", (long long)size);

    return 0;
}

void print_header(const Header *header){
    printf("magic: %.8s\n", header->magic);
    printf("version: %u\n", header->version);

    printf("\nNormal volume:\n");
    printf("offset: %lu\n", header->normal_offset);
    printf("size: %lu\n", header->normal_size);

    if (header->hidden_size > 0) {
        printf("\nhidden volume:\n");
        printf("offset: %lu\n", header->hidden_offset);
        printf("size: %lu\n", header->hidden_size);
    }
}

int info(const char *file_name) {
    int fd = open(file_name, O_RDONLY);
        if (fd == -1) {
            perror("cant open");
            return 1;
        }

        Header header = {0};
        
        if (read_header(fd, &header) != 0) {
            printf("not a SAFEUSB image.\n");
            close(fd);
            return 1;
        }

        print_header(&header);

        close(fd);

        return 0;
}

//int format(){}

int main(int argc, char *argv[]){
    if (argc < 2) {
        goto usage; }
    else if (strcmp(argv[1], "create") == 0) {
        if (argc != 4 && argc != 6)
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

    off_t hidden_size = 0;

    if (argc == 6 &&
        strcmp(argv[4], "--hidden") == 0)
    {
        hidden_size = parse_size(argv[5]);
    }

    return allocate(file_name, size, hidden_size);
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