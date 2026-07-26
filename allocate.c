// gcc -Wall -Wextra -o fake_usb allocate.c

#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

int main(){
    int fd = open("fake_usb.img", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd == -1) {
        perror("couldnt open");
        return 1;
    }

    off_t size = 5LL * 1024 * 1024 * 1024; // 5gb
    int err = posix_fallocate(fd, 0, size);
     if (err) {
        fprintf(stderr, "posix_fallocate failed: %d\n", err);
        close(fd);
        return 1;
    }

    close(fd);

    printf("Created fake_usb.img (%lld bytes)\n", (long long)size);
    
    return 0;
}