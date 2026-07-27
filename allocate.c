// gcc -Wall -Wextra -o fake_usb allocate.c
// ./fake_usb create fake_usb.img 5G (for no hidden)
// ./fake_usb create fake_usb.img 5G --hidden 2G
// ./fake_usb info fake_usb.img
// sudo ./fake_usb format fake_usb.img (keep running and mount in another terminal)
// sudo ./fake_usb mount fake_usb.img

#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// loop headers for formatting
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <sys/mount.h>


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

    write(fd, header, sizeof(*header));
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
    
    posix_fallocate(fd, 0, size);

    Header header = {0};

    memcpy(header.magic, "SAFEUSB", 8);
    header.version = 1;

    header.normal_offset = 4096;

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

int get_loop_device(){
    int control = open("/dev/loop-control", O_RDWR);

    if (control < 0) {
        perror("loop-control error");
        return -1;
    }

    int number = ioctl(control, LOOP_CTL_GET_FREE);

    close(control);

    return number;
}


int format_volume(const char *file_name, uint64_t offset, uint64_t size){

    // get free loop device
    int loop_num = get_loop_device();

    if (loop_num < 0) {
        return 1;
    }

    char loop_path[64];

    snprintf(loop_path, sizeof(loop_path), "/dev/loop%d", loop_num);

    printf("useing %s\n", loop_path);

    int img_fd = open(file_name, O_RDWR);

    int loop_fd = open(loop_path, O_RDWR);

    // configure loop device

    struct loop_config config = {0};

    config.fd = img_fd;
    config.block_size = 4096;

    config.info.lo_offset = offset;
    config.info.lo_sizelimit = size;

    //ioctl(loop_fd, LOOP_CONFIGURE, &config);
    if (ioctl(loop_fd, LOOP_CONFIGURE, &config) < 0) {
        perror("LOOP_CONFIGURE");
        close(loop_fd);
        close(img_fd);
        return 1;
    }

    // run mkfs.ext4

     pid_t pid = fork();

    if (pid == 0) {

        execlp(
            "mkfs.ext4",
            "mkfs.ext4",
            loop_path,
            NULL
        );

        perror("mkfs.ext4");
        exit(1);
    }

    wait(NULL);


    wait(NULL);

    printf("Filesystem created on %s\n", loop_path);
    printf("Press enter to detach loop device...\n");
    getchar();
    ioctl(loop_fd, LOOP_CLR_FD, 0);

    close(loop_fd);
    close(img_fd);
    return 0;
}

int format(const char *file_name){
    int fd = open(file_name, O_RDWR);
    
    Header header;

    if (read_header(fd, &header) != 0) {
        printf("Not a SAFEUSB image\n");
        close(fd);
        return 1;
    }

    format_volume(
        file_name,
        header.normal_offset,
        header.normal_size
    );


    // format hidden volume if it exists
    if (header.hidden_size > 0) {

        format_volume(
            file_name,
            header.hidden_offset,
            header.hidden_size
        );

    }

    close(fd);

    return 0;
}

int mount_volume(const char *file_name, uint64_t offset, uint64_t size, const char *mount_point)
{
    int loop_num = get_loop_device();

    if (loop_num < 0) {
        return 1;
    }

    char loop_path[64];

    snprintf(loop_path, sizeof(loop_path), "/dev/loop%d", loop_num);
    printf("using %s\n", loop_path);


    int img_fd = open(file_name, O_RDWR);

    if (img_fd < 0) {
        perror("image open");
        return 1;
    }

    int loop_fd = open(loop_path, O_RDWR);

    if (loop_fd < 0) {
        perror("loop open");
        close(img_fd);
        return 1;
    }

   struct loop_config config = {0};

    config.fd = img_fd;
    config.block_size = 4096;

    config.info.lo_offset = offset;
    config.info.lo_sizelimit = size;


    if (ioctl(loop_fd, LOOP_CONFIGURE, &config) < 0) {
        perror("LOOP_CONFIGURE");
        return 1;
    }

    // mount filesystem

    if (mount(
            loop_path,
            mount_point,
            "ext4",
            0,
            NULL
        ) < 0) {

        perror("mount error");
        return 1;
    }


    printf("\nmounted %s at %s\n", loop_path, mount_point);

    close(loop_fd);
    close(img_fd);

    return 0;
}

int mount(const char *file_name)
{
    int fd = open(file_name, O_RDONLY);

    Header header;

    if (read_header(fd, &header) != 0) {
        printf("Not a SAFEUSB image\n");
        close(fd);
        return 1;
    }


    mount_volume(
        file_name,
        header.normal_offset,
        header.normal_size,
        "/mnt"
    );
    close(fd);

    return 0;
}

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
    } else if (strcmp(argv[1], "format") == 0) {
        if (argc != 3)
            goto usage; 
    } 
    else if (strcmp(argv[1], "mount") == 0) {
        if (argc != 3)
            goto usage;
    } else {
        goto usage;
    }

    const char *file_name = argv[2];

    if (strcmp(argv[1], "create") == 0) {
        off_t size = parse_size(argv[3]);

    off_t hidden_size = 0;

    if (argc == 6 &&
        strcmp(argv[4], "--hidden") == 0){
        hidden_size = parse_size(argv[5]);
    }

    return allocate(file_name, size, hidden_size);
    
    } else if (strcmp(argv[1], "info") == 0) {
        return info(file_name);
    }
    else if (strcmp(argv[1], "format") == 0) {
        return format(file_name); 
    } 
    else if (strcmp(argv[1], "mount") == 0) {
        return mount(file_name);
    }
     else {
        goto usage;
    }

    usage:
        fprintf(stderr, "Usage: %s create <filename> <size[K|M|G]>\n", argv[0]);
        fprintf(stderr, "Usage: %s info <filename>\n", argv[0]);
        fprintf(stderr, "Usage: %s format <filename>\n", argv[0]);
        return 1;
}