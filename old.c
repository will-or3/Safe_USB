// gcc -Wall -Wextra -o fake_usb allocate.c
// ./fake_usb create fake_usb.img 5G (for no hidden)
// ./fake_usb create fake_usb.img 5G --hidden 2G
// ./fake_usb info fake_usb.img
// sudo ./fake_usb format fake_usb.img (keep running and mount in another terminal)
// sudo ./fake_usb mount fake_usb.img normal123
// sudo ./fake_usb mount fake_usb.img hidden123
// sudo ./fake_usb mount fake_usb.img duress123
// sudo ./fake_usb unmount

#define _FILE_OFFSET_BITS 64

#define HASH_LEN 32
#define SALT_LEN 16
#define SECTOR_SIZE 4096

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
#include <sys/random.h>

#include <argon2.h>
#include <sodium.h>

#include <ctype.h>

#define NONCE_LEN crypto_secretbox_NONCEBYTES
#define MAC_LEN crypto_secretbox_MACBYTES
#define KEY_LEN 64

if (sodium_init() < 0) {
    fprintf(stderr, "libsodium init failed\n");
    return 1;
}

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

int derive_key(
    const char *password,
    const uint8_t *salt,
    uint8_t *key
){
    return argon2id_hash_raw(
        3,              // iterations
        1 << 16,        // 64 MB
        1,              // threads
        password,
        strlen(password),
        salt,
        SALT_LEN,
        key,
        KEY_LEN
    );
}

void hash_password(
    const char *password,
    const uint8_t salt[SALT_LEN],
    uint8_t hash[HASH_LEN]){
    
        argon2id_hash_raw(
        3,         
        1 << 16,      
        1,             
        password,
        strlen(password),
        salt,
        SALT_LEN,
        hash,
        HASH_LEN
    );
}

int encrypt_volume_key(
    const uint8_t *master_key,
    const uint8_t *volume_key,
    uint8_t *nonce,
    uint8_t *ciphertext ){
    
    random_bytes(nonce, NONCE_LEN);

    crypto_secretbox_easy(
        ciphertext,
        volume_key,
        KEY_LEN,
        nonce,
        master_key
    );

    return 0;
}

int decrypt_volume_key(
    const uint8_t *master_key,
    const uint8_t *nonce,
    const uint8_t *ciphertext,
    uint8_t *volume_key
){
    if (crypto_secretbox_open_easy(
            volume_key,
            ciphertext,
            KEY_LEN + MAC_LEN,
            nonce,
            master_key) != 0)
    {
        return -1;      // wrong password
    }

    return 0;
}

int verify_password(
    const char *password,
    const uint8_t salt[SALT_LEN],
    const uint8_t expected_hash[HASH_LEN]){
    
    uint8_t hash[HASH_LEN];
    hash_password(password, salt, hash);

    return memcmp(hash, expected_hash, HASH_LEN) == 0;
}


void random_bytes(void *buf, size_t len){
    randombytes_buf(buf, len);
}

void encrypt_sector(
    const uint8_t volume_key[KEY_LEN],
    const uint8_t volume_nonce[NONCE_LEN],
    uint64_t sector,
    uint8_t *buffer,
    size_t length){
    
    uint8_t nonce[NONCE_LEN];

    memcpy(nonce, volume_nonce, NONCE_LEN);

    memcpy(nonce + NONCE_LEN - sizeof(sector), &sector, sizeof(sector));

    crypto_stream_xchacha20_xor(
        buffer,
        buffer,
        length,
        nonce,
        volume_key
    );
}

void decrypt_sector(
    const uint8_t volume_key[KEY_LEN],
    const uint8_t volume_nonce[NONCE_LEN],
    uint64_t sector,
    uint8_t *buffer,
    size_t length){
    
    encrypt_sector(
        volume_key,
        volume_nonce,
        sector,
        buffer,
        length
    );
}

void key_to_hex(
    const uint8_t key[KEY_LEN],
    char hex[KEY_LEN * 2 + 1])
{
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < KEY_LEN; i++) {
        hex[i * 2]     = digits[key[i] >> 4];
        hex[i * 2 + 1] = digits[key[i] & 0x0F];
    }

    hex[KEY_LEN * 2] = '\0';
}

int create_dm_mapping(
    const char *loop_path,
    const uint8_t volume_key[KEY_LEN],
    uint64_t sectors,
    const char *mapper_name);

int read_sector(
    int fd,
    uint64_t volume_offset,
    uint64_t sector,
    uint8_t buffer[SECTOR_SIZE],
    const uint8_t volume_key[KEY_LEN],
    const uint8_t volume_nonce[NONCE_LEN]){

    off_t offset = volume_offset + sector * SECTOR_SIZE;

    lseek(fd, offset, SEEK_SET);

    if (read(fd, buffer, SECTOR_SIZE) != SECTOR_SIZE)
        return -1;

    decrypt_sector(
        volume_key,
        volume_nonce,
        sector,
        buffer,
        SECTOR_SIZE
    );

    return 0;
}

int write_sector(
    int fd,
    uint64_t volume_offset,
    uint64_t sector,
    uint8_t buffer[SECTOR_SIZE],
    const uint8_t volume_key[KEY_LEN],
    const uint8_t volume_nonce[NONCE_LEN]){

    off_t offset = volume_offset + sector * SECTOR_SIZE;

    encrypt_sector(
        volume_key,
        volume_nonce,
        sector,
        buffer,
        SECTOR_SIZE
    );

    lseek(fd, offset, SEEK_SET);

    if (write(fd, buffer, SECTOR_SIZE) != SECTOR_SIZE)
        return -1;

    decrypt_sector(
        volume_key,
        volume_nonce,
        sector,
        buffer,
        SECTOR_SIZE
    );

    return 0;
}

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

    // hash
    random_bytes(header.normal_salt, SALT_LEN);
    random_bytes(header.hidden_salt, SALT_LEN);
    random_bytes(header.duress_salt, SALT_LEN);

    hash_password(
        "normal123",
        header.normal_salt,
        header.normal_hash
    );

    hash_password(
        "hidden123",
        header.hidden_salt,
        header.hidden_hash
    );

    hash_password(
        "duress123",
        header.duress_salt,
        header.duress_hash
    );

    // normal

    uint8_t master_key[KEY_LEN];
    uint8_t volume_key[KEY_LEN];

    random_bytes(volume_key, KEY_LEN);

    derive_key(
        "normal123",
        header.normal_salt,
        master_key
    );

    encrypt_volume_key(
        master_key,
        volume_key,
        header.normal_nonce,
        header.normal_key_enc
    );

    // hidden

    random_bytes(volume_key, KEY_LEN);

    derive_key(
        "hidden123",
        header.hidden_salt,
        master_key
    );

    encrypt_volume_key(
        master_key,
        volume_key,
        header.hidden_nonce,
        header.hidden_key_enc
    );


    // save header
    write_header(fd, &header);

    close(fd);

    printf("Created fake_usb.img (%lld bytes)\n",
           (long long)size);

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

// loop helper
int setup_loop(
    const char *image,
    uint64_t offset,
    uint64_t size,
    char *loop_path,
    size_t path_size
){
    int loop_num = get_loop_device();
    
    snprintf(
        loop_path,
        path_size,
        "/dev/loop%d",
        loop_num
    );

    int img_fd = open(image, O_RDWR);
    int loop_fd = open(loop_path, O_RDWR);

    struct loop_config config = {0};

    config.fd = img_fd;
    config.block_size = 4096;

    config.info.lo_offset = offset;
    config.info.lo_sizelimit = size;

    ioctl(loop_fd, LOOP_CONFIGURE, &config);

    close(loop_fd);
    close(img_fd);

    return 0;
}

int detach_loop(const char *loop_path){
    int fd = open(loop_path, O_RDWR);

    if (fd < 0) {
        perror("open loop");
        return 1;
    }

    ioctl(fd, LOOP_CLR_FD, 0);
    close(fd);

    return 0;
}

int format_volume(const char *file_name, uint64_t offset, uint64_t size){

    char loop_path[64];

    setup_loop(file_name, offset, size, loop_path, sizeof(loop_path));

    pid_t pid = fork();

    if (pid == 0) {
        execlp("mkfs.ext4",
            "mkfs.ext4",
            loop_path,
            NULL);

        perror("mkfs.ext4");
        exit(1);
    }

    
    waitpid(pid, NULL, 0);

    detach_loop(loop_path);

    return 0;

}

int format_img(const char *file_name){
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


int mount_volume(
    const char *file_name, 
    uint64_t offset, 
    uint64_t size, 
    const char *mount_point,
    const char *loop_file){
    
    char loop_path[64];

    setup_loop(file_name, offset, size, loop_path, sizeof(loop_path));

    mount(
        loop_path,
        mount_point,
        "ext4",
        0,
        NULL
    );

     printf("mounted %s at %s\n", loop_path, mount_point);

    FILE *f = fopen(loop_file, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }

    fprintf(f, "%s\n", loop_path);
    fclose(f);

// don't detach here

    return 0;
}

void duress_action(void);

int unlock_volume(
    Header *header, const char *password, 
    uint8_t volume_key[KEY_LEN], UnlockMode *mode){
    
    uint8_t master_key[KEY_LEN];

    if (verify_password(
            password, header->normal_salt, header->normal_hash)){
        
            if (derive_key( password,
                header->normal_salt,
                master_key) != ARGON2_OK)
            return -1;

        if (decrypt_volume_key(
                master_key,
                header->normal_nonce,
                header->normal_key_enc,
                volume_key) != 0)
            return -1;

        *mode = MODE_NORMAL;
        return 0;
    }

    if (verify_password(
            password,
            header->hidden_salt,
            header->hidden_hash))
    {
        if (derive_key(
                password,
                header->hidden_salt,
                master_key) != ARGON2_OK)
            return -1;

        if (decrypt_volume_key(
                master_key,
                header->hidden_nonce,
                header->hidden_key_enc,
                volume_key) != 0)
            return -1;

        *mode = MODE_HIDDEN;
        return 0;
    }

    if (verify_password(
            password,
            header->duress_salt,
            header->duress_hash))
    {
        *mode = MODE_DURESS;
        return 0;
    }

    *mode = MODE_NONE;
    return -1;
}


int mount_img(const char *file_name, const char *password){
    int fd = open(file_name, O_RDONLY);

    Header header;

    if (read_header(fd, &header) != 0) {
        printf("Not a SAFEUSB image\n");
        close(fd);
        return 1;
    }

    uint8_t volume_key[KEY_LEN];
    UnlockMode mode;

    if (unlock_volume(
            &header,
            password,
            volume_key,
            &mode) != 0)
    {
        printf("Incorrect password\n");
        close(fd);
        return 1;
    }

    close(fd);

    switch (mode) {

    case MODE_NORMAL:
        return mount_volume(
            file_name,
            header.normal_offset,
            header.normal_size,
            "/mnt",
            "/tmp/safeusb.loop"
        );

    case MODE_HIDDEN:
        if (header.hidden_size == 0) {
            printf("No hidden volume exists\n");
            return 1;
        }

        return mount_volume(
            file_name,
            header.hidden_offset,
            header.hidden_size,
            "/mnt",
            "/tmp/safeusb.loop"
        );

    case MODE_DURESS:
        duress_action();
        return 0;

    default:
        printf("Incorrect password\n");
        return 1;
    }

}


int unmount_volume(const char *mount_point, const char *loop_file){
    
    char loop_path[64];

    FILE *f = fopen(loop_file, "r");
    if (!f) {
        perror("fopen");
        return 1;
    }

    if (fgets(loop_path, sizeof(loop_path), f) == NULL) {
        fclose(f);
        fprintf(stderr, "couldnt read loop file\n");
        return 1;
    }

    fclose(f);

    loop_path[strcspn(loop_path, "\n")] = '\0';

    umount(mount_point);

    detach_loop(loop_path);

    remove(loop_file);

    printf("unmounted %s\n", loop_path);

    return 0;
}

void duress_action(){
    printf("Duress action triggered\n");
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
        if (argc != 4)
            goto usage;
    } else if (strcmp(argv[1], "unmount") == 0) {
        if (argc != 2)
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
        return format_img(file_name); 
    } 
    else if (strcmp(argv[1], "mount") == 0) {
        return mount_img(file_name, argv[3]);
    } else if (strcmp(argv[1], "unmount") == 0) {
        return unmount_volume("/mnt", "/tmp/safeusb.loop");
    }
     else {
        goto usage;
    }

    usage:
        fprintf(stderr, "Usage: %s create <filename> <size[K|M|G]>\n", argv[0]);
        fprintf(stderr, "Usage: %s info <filename>\n", argv[0]);
        fprintf(stderr, "Usage: %s format <filename>\n", argv[0]);
        fprintf(stderr, "Usage: %s mount <filename> normal/hidden\n", argv[0]);
        fprintf(stderr, "Usage: %s unmount\n", argv[0]);
        return 1;
}