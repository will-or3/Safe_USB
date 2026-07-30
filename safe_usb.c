// safe_usb.c
// Build: gcc -Wall -Wextra -o safe_usb safe_usb.c -largon2 -lsodium
// Usage:
//   ./safe_usb auto <target> <size> <password> [--hidden hidden_size]
//       size required when creating a new file image, ignored for block devices.
//   ./safe_usb create <file> <size> [--hidden <hidden_size>]
//   ./safe_usb info <file>
//   ./safe_usb format <file>
//   ./safe_usb mount <file> <password>
//   ./safe_usb unmount

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/random.h>
#include <linux/loop.h>
#include <linux/fs.h>       // for BLKGETSIZE64
#include <argon2.h>
#include <sodium.h>
#include <ctype.h>

#define HASH_LEN 32
#define SALT_LEN 16
#define SECTOR_SIZE 4096

#define NONCE_LEN crypto_secretbox_NONCEBYTES
#define MAC_LEN  crypto_secretbox_MACBYTES
#define KEY_LEN   64          // 512 bits for AES-256-XTS

// header
typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;

    uint64_t normal_offset;
    uint64_t normal_size;

    uint64_t hidden_offset;
    uint64_t hidden_size;

    uint8_t normal_hash[HASH_LEN];
    uint8_t normal_salt[SALT_LEN];

    uint8_t hidden_hash[HASH_LEN];
    uint8_t hidden_salt[SALT_LEN];

    uint8_t duress_hash[HASH_LEN];
    uint8_t duress_salt[SALT_LEN];

    uint8_t normal_nonce[NONCE_LEN];
    uint8_t hidden_nonce[NONCE_LEN];

    uint8_t normal_key_enc[KEY_LEN + MAC_LEN];
    uint8_t hidden_key_enc[KEY_LEN + MAC_LEN];
} Header;

typedef enum {
    MODE_NONE,
    MODE_NORMAL,
    MODE_HIDDEN,
    MODE_DURESS
} UnlockMode;

// crypto helpers
void random_bytes(void *buf, size_t len) {
    randombytes_buf(buf, len);
}

int derive_key(const char *password, const uint8_t *salt, uint8_t *key) {
    return argon2id_hash_raw(3, 1<<16, 1, password, strlen(password),
                             salt, SALT_LEN, key, KEY_LEN);
}

void hash_password(const char *password, const uint8_t salt[SALT_LEN],
                   uint8_t hash[HASH_LEN]) {
    argon2id_hash_raw(3, 1<<16, 1, password, strlen(password), 
                      salt, SALT_LEN,hash, HASH_LEN);
}

int verify_password(const char *password, const uint8_t salt[SALT_LEN],
                    const uint8_t expected_hash[HASH_LEN]) {
    uint8_t hash[HASH_LEN];
    hash_password(password, salt, hash);
    return memcmp(hash, expected_hash, HASH_LEN) == 0;
}

int encrypt_volume_key(const uint8_t *master_key, const uint8_t *volume_key,
                       uint8_t *nonce, uint8_t *ciphertext) {
    random_bytes(nonce, NONCE_LEN);
    crypto_secretbox_easy(ciphertext, volume_key, KEY_LEN, nonce, master_key);
    return 0;
}

int decrypt_volume_key(const uint8_t *master_key, const uint8_t *nonce,
                       const uint8_t *ciphertext, uint8_t *volume_key) {
    if (crypto_secretbox_open_easy(volume_key, ciphertext, KEY_LEN + MAC_LEN,
                                   nonce, master_key) != 0)
        return -1;
    return 0;
}

void key_to_hex(const uint8_t key[KEY_LEN], char hex[KEY_LEN * 2 + 1]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < KEY_LEN; i++) {
        hex[i*2]     = digits[key[i] >> 4];
        hex[i*2 + 1] = digits[key[i] & 0x0F];
    }
    hex[KEY_LEN*2] = '\0';
}

// dmcrypt mapping with dmstep
int create_dm_mapping(const char *loop_path, const uint8_t volume_key[KEY_LEN],
                      uint64_t sectors, const char *mapper_name) {
    char key_hex[KEY_LEN*2 + 1];
    key_to_hex(volume_key, key_hex);

    char table[512];
    snprintf(table, sizeof(table),
             "0 %llu crypt aes-xts-plain64 %s 0 %s 0",
             (unsigned long long)sectors, key_hex, loop_path);

    int pipefd[2];
    if (pipe(pipefd) == -1) { perror("pipe"); return -1; }

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        execlp("dmsetup", "dmsetup", "create", mapper_name, NULL);
        perror("execlp dmsetup");
        _exit(1);
    }
    close(pipefd[0]);
    write(pipefd[1], table, strlen(table));
    close(pipefd[1]);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int remove_dm_mapping(const char *mapper_name) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("dmsetup", "dmsetup", "remove", mapper_name, NULL);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

// loop device helpers
int get_loop_device(void) {
    int ctl = open("/dev/loop-control", O_RDWR);
    if (ctl < 0) { perror("loop-control"); return -1; }
    int num = ioctl(ctl, LOOP_CTL_GET_FREE);
    close(ctl);
    return num;
}

int setup_loop(const char *image, uint64_t offset, uint64_t size,
               char *loop_path, size_t path_size) {
    int loop_num = get_loop_device();
    if (loop_num < 0) return -1;
    snprintf(loop_path, path_size, "/dev/loop%d", loop_num);

    int img_fd = open(image, O_RDWR);
    if (img_fd < 0) { perror("open image"); return -1; }
    int loop_fd = open(loop_path, O_RDWR);
    if (loop_fd < 0) { perror("open loop"); close(img_fd); return -1; }

    struct loop_config cfg = {0};
    cfg.fd = img_fd;
    cfg.block_size = 4096;
    cfg.info.lo_offset = offset;
    cfg.info.lo_sizelimit = size;

    if (ioctl(loop_fd, LOOP_CONFIGURE, &cfg) < 0) {
        perror("LOOP_CONFIGURE");
        close(loop_fd); close(img_fd);
        return -1;
    }
    close(loop_fd);
    close(img_fd);
    return 0;
}

int detach_loop(const char *loop_path) {
    int fd = open(loop_path, O_RDWR);
    if (fd < 0) { perror("open loop"); return -1; }
    if (ioctl(fd, LOOP_CLR_FD, 0) < 0) {
        perror("LOOP_CLR_FD");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

// header io
int write_header(int fd, Header *header) {
    lseek(fd, 0, SEEK_SET);
    if (write(fd, header, sizeof(*header)) != sizeof(*header))
        return -1;
    fsync(fd);
    return 0;
}

int read_header(int fd, Header *header) {
    lseek(fd, 0, SEEK_SET);
    if (read(fd, header, sizeof(*header)) != sizeof(*header))
        return -1;
    if (memcmp(header->magic, "SAFEUSB", 7) != 0)
        return -1;
    return 0;
}

void print_header(const Header *header) {
    printf("magic: %.8s\n", header->magic);
    printf("version: %u\n", header->version);
    printf("\nNormal volume:\n");
    printf("  offset: %lu\n", header->normal_offset);
    printf("  size: %lu\n", header->normal_size);
    if (header->hidden_size > 0) {
        printf("\nHidden volume:\n");
        printf("  offset: %lu\n", header->hidden_offset);
        printf("  size: %lu\n", header->hidden_size);
    }
}

// create image, regular files only
off_t parse_size(const char *str) {
    char *end;
    long long val = strtoll(str, &end, 10);
    if (end == str || val <= 0) return 0;
    switch (*end) {
        case 'G': case 'g': return (off_t)val * 1024*1024*1024;
        case 'M': case 'm': return (off_t)val * 1024*1024;
        case 'K': case 'k': return (off_t)val * 1024;
        case '\0': return (off_t)val;
        default: return 0;
    }
}

int allocate(const char *file, off_t total, off_t hidden) {
    int fd = open(file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) { perror("open"); return 1; }
    if (posix_fallocate(fd, 0, total) != 0) {
        perror("fallocate"); close(fd); return 1;
    }

    Header hdr = {0};
    memcpy(hdr.magic, "SAFEUSB", 8);
    hdr.version = 1;
    hdr.normal_offset = 4096;

    if (hidden > 0) {
        hdr.hidden_size = hidden;
        hdr.normal_size = total - 4096 - hidden;
        hdr.hidden_offset = hdr.normal_offset + hdr.normal_size;
    } else {
        hdr.normal_size = total - 4096;
        hdr.hidden_size = 0;
        hdr.hidden_offset = 0;
    }

    // dflt passwords
    random_bytes(hdr.normal_salt, SALT_LEN);
    random_bytes(hdr.hidden_salt, SALT_LEN);
    random_bytes(hdr.duress_salt, SALT_LEN);
    hash_password("normal123", hdr.normal_salt, hdr.normal_hash);
    hash_password("hidden123", hdr.hidden_salt, hdr.hidden_hash);
    hash_password("duress123", hdr.duress_salt, hdr.duress_hash);

    uint8_t master[KEY_LEN], vol_key[KEY_LEN];

    // normal
    random_bytes(vol_key, KEY_LEN);
    derive_key("normal123", hdr.normal_salt, master);
    encrypt_volume_key(master, vol_key, hdr.normal_nonce, hdr.normal_key_enc);

    // hidden
    random_bytes(vol_key, KEY_LEN);
    derive_key("hidden123", hdr.hidden_salt, master);
    encrypt_volume_key(master, vol_key, hdr.hidden_nonce, hdr.hidden_key_enc);

    write_header(fd, &hdr);
    close(fd);
    printf("Created %s (%lld bytes)\n", file, (long long)total);
    return 0;
}

// write header on an existing file descriptor,for block devices
int write_initial_header(int fd, off_t total, off_t hidden) {
    Header hdr = {0};
    memcpy(hdr.magic, "SAFEUSB", 8);
    hdr.version = 1;
    hdr.normal_offset = 4096;

    if (hidden > 0) {
        hdr.hidden_size = hidden;
        hdr.normal_size = total - 4096 - hidden;
        hdr.hidden_offset = hdr.normal_offset + hdr.normal_size;
    } else {
        hdr.normal_size = total - 4096;
        hdr.hidden_size = 0;
        hdr.hidden_offset = 0;
    }

    random_bytes(hdr.normal_salt, SALT_LEN);
    random_bytes(hdr.hidden_salt, SALT_LEN);
    random_bytes(hdr.duress_salt, SALT_LEN);
    hash_password("normal123", hdr.normal_salt, hdr.normal_hash);
    hash_password("hidden123", hdr.hidden_salt, hdr.hidden_hash);
    hash_password("duress123", hdr.duress_salt, hdr.duress_hash);

    uint8_t master[KEY_LEN], vol_key[KEY_LEN];

    random_bytes(vol_key, KEY_LEN);
    derive_key("normal123", hdr.normal_salt, master);
    encrypt_volume_key(master, vol_key, hdr.normal_nonce, hdr.normal_key_enc);

    random_bytes(vol_key, KEY_LEN);
    derive_key("hidden123", hdr.hidden_salt, master);
    encrypt_volume_key(master, vol_key, hdr.hidden_nonce, hdr.hidden_key_enc);

    return write_header(fd, &hdr);
}

// format volume with mkfs ext4, linux standard, I wonder, if i made it
//  multi platform would the filesystem matter ? like obviously ext4 is linux only
// and all the code is linux only but if I made it multi platform would the filesystem matter
// I guess it probably would because the encryption doesn't matter when the filesystem is actually being exposed to the os
int format_volume(const char *file, uint64_t offset, uint64_t size) {
    char loop[64];
    if (setup_loop(file, offset, size, loop, sizeof(loop)) != 0)
        return -1;

    // you dont need this on .img files because they dont have existing file systems
    // but usb's might have filesystems before, learned this the hard way
    pid_t pid = fork();
    if (pid == 0) {
        execlp("wipefs", "wipefs", "-a", loop, NULL);
        perror("wipefs");
        _exit(1);
    }
    waitpid(pid, NULL, 0);

    pid_t pid = fork();
    if (pid == 0) {
        execlp("mkfs.ext4", "mkfs.ext4", loop, NULL);
        perror("mkfs.ext4");
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    detach_loop(loop);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int format_img(const char *file) {
    int fd = open(file, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    Header hdr;
    if (read_header(fd, &hdr) != 0) {
        fprintf(stderr, "Not a SAFEUSB image\n");
        close(fd);
        return 1;
    }
    close(fd);

    printf("Formatting normal volume...\n");
    if (format_volume(file, hdr.normal_offset, hdr.normal_size) != 0)
        return 1;

    if (hdr.hidden_size > 0) {
        printf("Formatting hidden volume...\n");
        if (format_volume(file, hdr.hidden_offset, hdr.hidden_size) != 0)
            return 1;
    }
    return 0;
}

// unlock volumes with salt and header
int unlock_volume(Header *hdr, const char *password,
                  uint8_t volume_key[KEY_LEN], UnlockMode *mode) {
    uint8_t master[KEY_LEN];

    if (verify_password(password, hdr->normal_salt, hdr->normal_hash)) {
        if (derive_key(password, hdr->normal_salt, master) != ARGON2_OK)
            return -1;
        if (decrypt_volume_key(master, hdr->normal_nonce,
                               hdr->normal_key_enc, volume_key) != 0)
            return -1;
        *mode = MODE_NORMAL;
        return 0;
    }
    if (verify_password(password, hdr->hidden_salt, hdr->hidden_hash)) {
        if (derive_key(password, hdr->hidden_salt, master) != ARGON2_OK)
            return -1;
        if (decrypt_volume_key(master, hdr->hidden_nonce,
                               hdr->hidden_key_enc, volume_key) != 0)
            return -1;
        *mode = MODE_HIDDEN;
        return 0;
    }
    if (verify_password(password, hdr->duress_salt, hdr->duress_hash)) {
        *mode = MODE_DURESS;
        return 0;
    }
    *mode = MODE_NONE;
    return -1;
}

// mount with dmcrypt
int mount_volume(const char *image, uint64_t offset, uint64_t size,
                 const char *mountpoint, const char *state_file,
                 const uint8_t volume_key[KEY_LEN]) {
    char loop[64];
    if (setup_loop(image, offset, size, loop, sizeof(loop)) != 0)
        return 1;

    const char *mapper = "safeusb";
    if (create_dm_mapping(loop, volume_key, size / 512, mapper) != 0) {
        detach_loop(loop);
        return 1;
    }

    char dm_path[128];
    snprintf(dm_path, sizeof(dm_path), "/dev/mapper/%s", mapper);

    if (mount(dm_path, mountpoint, "ext4", 0, NULL) != 0) {
        perror("mount");
        remove_dm_mapping(mapper);
        detach_loop(loop);
        return 1;
    }

    FILE *f = fopen(state_file, "w");
    if (f) {
        fprintf(f, "%s\n%s\n", loop, mapper);
        fclose(f);
    }
    printf("Mounted %s on %s\n", dm_path, mountpoint);
    return 0;
}

void duress_action(void) {
    int fd = open(file, O_RDWR);

    Header hdr;
    if (read_header(fd, &hdr) != 0) {
        fprintf(stderr, "self destruct not a SAFEUSB image\n");
        close(fd);
        return;
    }

    // overwrite
    random_bytes(hdr.hidden_nonce, sizeof(hdr.hidden_nonce));
    random_bytes(hdr.hidden_key_enc, sizeof(hdr.hidden_key_enc));
    random_bytes(hdr.hidden_hash, sizeof(hdr.hidden_hash));
    random_bytes(hdr.hidden_salt, sizeof(hdr.hidden_salt));

    if (write_header(fd, &hdr) != 0) {
        fprintf(stderr, "Self-destruct: failed to write header\n");
    } else {
        printf("Hidden volume permanently destroyed.\n");
    }

    close(fd);
}

int mount_img(const char *file, const char *password) {
    int fd = open(file, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    Header hdr;
    if (read_header(fd, &hdr) != 0) {
        fprintf(stderr, "Not a SAFEUSB image\n");
        close(fd);
        return 1;
    }
    close(fd);

    uint8_t volume_key[KEY_LEN];
    UnlockMode mode;
    if (unlock_volume(&hdr, password, volume_key, &mode) != 0) {
        printf("Incorrect password\n");
        return 1;
    }

    switch (mode) {
    case MODE_NORMAL:
        return mount_volume(file, hdr.normal_offset, hdr.normal_size,
                            "/mnt", "/tmp/safeusb.loop", volume_key);
    case MODE_HIDDEN:
        if (hdr.hidden_size == 0) {
            printf("No hidden volume exists\n");
            return 1;
        }
        return mount_volume(file, hdr.hidden_offset, hdr.hidden_size,
                            "/mnt", "/tmp/safeusb.loop", volume_key);
    case MODE_DURESS:
        duress_action(file_name);
        return 0;
    default:
        return 1;
    }
}

int unmount_volume(const char *mountpoint, const char *state_file) {
    char loop[64], mapper[64];
    FILE *f = fopen(state_file, "r");
    if (!f) { perror("open state"); return 1; }
    if (fgets(loop, sizeof(loop), f) == NULL) { fclose(f); return 1; }
    if (fgets(mapper, sizeof(mapper), f) == NULL) { fclose(f); return 1; }
    fclose(f);

    loop[strcspn(loop, "\n")] = 0;
    mapper[strcspn(mapper, "\n")] = 0;

    if (umount(mountpoint) != 0) perror("umount");
    remove_dm_mapping(mapper);
    detach_loop(loop);
    unlink(state_file);
    printf("Unmounted.\n");
    return 0;
}

// checks
int probe_and_init(const char *path, off_t hidden_size, off_t fallback_total) {
    struct stat st;
    int exists = stat(path, &st) == 0;

    if (!exists) {
        // Create a new file image
        return allocate(path, fallback_total, hidden_size);
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    Header hdr;
    if (read_header(fd, &hdr) == 0) {
        // ours
        close(fd);
        return 0;
    }

    // not mine, write header
    off_t total;
    if (S_ISBLK(st.st_mode)) {
        if (ioctl(fd, BLKGETSIZE64, &total) < 0) {
            perror("BLKGETSIZE64");
            close(fd);
            return 1;
        }
    } else {
        total = st.st_size;
    }

    if (write_initial_header(fd, total, hidden_size) != 0) {
        close(fd);
        return 1;
    }
    close(fd);

    printf("Initialised header. Formatting...\n");
    return format_img(path);
}


int main(int argc, char *argv[]) {
    if (argc < 2) goto usage;

    if (sodium_init() < 0) {
        fprintf(stderr, "libsodium init failed\n");
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "auto") == 0) {
        // auto <target> <size> <password> [--hidden <hidden_size>]
        if (argc < 4) goto usage;
        const char *target = argv[2];
        const char *size_str = argv[3];
        const char *password = argv[4];

        off_t fallback_total = parse_size(size_str);
        if (fallback_total <= 0) {
            fprintf(stderr, "invalid size:%s\n", size_str);
            return 1;
        }

        off_t hidden_size = 0;
        if (argc >= 6 && strcmp(argv[5], "--hidden") == 0) {
            if (argc < 7) goto usage;
            hidden_size = parse_size(argv[6]);
            if (hidden_size <= 0) {
                fprintf(stderr, "invalid hidden size:%s\n", argv[6]);
                return 1;
            }
        }

        if (probe_and_init(target, hidden_size, fallback_total) != 0) {
            fprintf(stderr, "initialisation failed\n");
            return 1;
        }

        // mount with the given password
        return mount_img(target, password);
    }

    if (strcmp(cmd, "create") == 0) {
        if (argc < 4) goto usage;
        off_t total = parse_size(argv[3]);
        if (total <= 0) { fprintf(stderr, "bad size\n"); return 1; }
        off_t hidden = 0;
        if (argc >= 5 && strcmp(argv[4], "--hidden") == 0) {
            if (argc < 6) goto usage;
            hidden = parse_size(argv[5]);
            if (hidden <= 0) { fprintf(stderr, "bad hidden size\n"); return 1; }
        }
        return allocate(argv[2], total, hidden);
    }

    if (strcmp(cmd, "info") == 0) {
        if (argc != 3) goto usage;
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) { perror("open"); return 1; }
        Header hdr;
        if (read_header(fd, &hdr) != 0) {
            printf("Not a SAFEUSB image.\n");
            close(fd);
            return 1;
        }
        print_header(&hdr);
        close(fd);
        return 0;
    }

    if (strcmp(cmd, "format") == 0) {
        if (argc != 3) goto usage;
        return format_img(argv[2]);
    }

    if (strcmp(cmd, "mount") == 0) {
        if (argc != 4) goto usage;
        return mount_img(argv[2], argv[3]);
    }

    if (strcmp(cmd, "unmount") == 0) {
        return unmount_volume("/mnt", "/tmp/safeusb.loop");
    }

usage:
    fprintf(stderr,
        "Usage:\n"
        "  %s auto   <target> <size> <password> [--hidden <hidden>]\n"
        "  %s create <file> <size> [--hidden <hidden>]\n"
        "  %s info   <file>\n"
        "  %s format <file>\n"
        "  %s mount  <file> <password>\n"
        "  %s unmount\n",
        argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 1;
}