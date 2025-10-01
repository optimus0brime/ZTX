#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <signal.h>

#define BUF_MAX   (256 * 1024 * 1024) // 256MiB default
#define LOG_FILE  "wipe_log.csv"
#define DEV_STR   128

// Zero-and-free macro for security
#define ZERO_FREE(p, sz) if(p){ memset(p, 0, sz); free(p); p = NULL; }

// Partial write handler
ssize_t full_write(int fd, const void *buf, size_t count) {
    size_t left = count;
    const char *p = buf;
    while (left > 0) {
        ssize_t done = write(fd, p, left);
        if (done < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= done; p += done;
    }
    return count;
}

// Get device size in bytes
long get_dev_size(const char *dev) {
    char cmd[DEV_STR], buf[DEV_STR];
    snprintf(cmd, sizeof(cmd), "lsblk -nb -o SIZE %s", dev);
    FILE *fp = popen(cmd, "r"); if (!fp) return -1;
    long sz = (fgets(buf, sizeof(buf), fp)) ? atol(buf) : -1;
    pclose(fp); return sz;
}

// Check if device is SSD (ROTA == 0)
int is_ssd(const char *dev) {
    char cmd[DEV_STR], buf[32];
    snprintf(cmd, sizeof(cmd), "lsblk -dn -o ROTA %s", dev);
    FILE *fp = popen(cmd, "r"); if (!fp) return 0;
    int ret = (fgets(buf, sizeof(buf), fp) && atoi(buf) == 0);
    pclose(fp); return ret;
}

// Progress bar UI
void draw_progress(WINDOW *win, int y, int x, double frac, int w) {
    int prog = (int)(w * frac);
    mvwprintw(win, y, x, "[");
    for (int i = 0; i < w; i++) waddch(win, (i < prog) ? '#' : '-');
    wprintw(win, "] %3.1f%%", frac * 100.0);
    wrefresh(win);
}

// Secure disk wipe
int secure_wipe(const char *dev) {
    long size = get_dev_size(dev);
    if (size <= 0) { printf("Error: Could not get device size.\n"); return 0; }

    size_t bufsize = BUF_MAX;
    if (bufsize > size) bufsize = size;
    unsigned char *buf = NULL;
    if (posix_memalign((void**)&buf, 4096, bufsize)) {
        printf("Error: Buffer allocation failed.\n"); return 0;
    }

    // Secure buffer & OpenSSL context
    unsigned char key[32], iv[16];
    FILE *rnd = fopen("/dev/urandom", "rb");
    if (!rnd || fread(key,1,32,rnd)!=32 || fread(iv,1,16,rnd)!=16) {
        printf("Error: urandom read failed.\n"); if(rnd)fclose(rnd); ZERO_FREE(buf, bufsize); return 0;
    }
    fclose(rnd);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx || !EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv)) {
        printf("Crypto init failed.\n"); EVP_CIPHER_CTX_free(ctx); ZERO_FREE(buf, bufsize); return 0;
    }

    int fd = open(dev, O_WRONLY | O_SYNC);
    if (fd < 0) {
        printf("open(%s) failed: %s\n", dev, strerror(errno));
        EVP_CIPHER_CTX_free(ctx); ZERO_FREE(buf, bufsize); return 0;
    }

    WINDOW *win = NULL;
    initscr(); noecho(); cbreak(); curs_set(0); win = newwin(12, COLS-4, (LINES-12)/2, 2);
    long written = 0;
    time_t start = time(NULL);
    int cancelled = 0;

    while (written < size) {
        size_t chunk = (size - written > bufsize) ? bufsize : (size - written);
        memset(buf, 0, chunk);
        int outlen = 0;
        if (!EVP_EncryptUpdate(ctx, buf, &outlen, buf, chunk)) {
            mvwprintw(win, 5, 2, "Crypto error"); cancelled = 1; break;
        }
        ssize_t n = full_write(fd, buf, chunk);
        if (n < 0) {
            mvwprintw(win, 5, 2, "Write error: %s", strerror(errno));
            cancelled = 1; break;
        }
        written += n;
        double frac = (double)written / size;
        double dt = difftime(time(NULL), start);
        double speed = (dt > 0) ? (written/1048576.0)/dt : 0;
        double eta = (speed > 0) ? ((size-written)/1048576.0)/speed : 0;

        werase(win); box(win,0,0);
        mvwprintw(win, 1, 2, "Secure Wipe: %s", dev);
        draw_progress(win, 3, 4, frac, getmaxx(win)-14);
        mvwprintw(win, 5, 2, "Written: %.2f / %.2f GiB", written/1073741824.0, size/1073741824.0);
        mvwprintw(win, 6, 2, "Speed: %.1f MiB/s | ETA: %.0f sec", speed, eta);
        mvwprintw(win, 8, 2, "Q/ESC = Abort"); wrefresh(win);
        timeout(250); int ch = wgetch(win);
        if (ch == 'q' || ch == 'Q' || ch == 27) { cancelled = 2; break; }
    }

    fsync(fd);
    close(fd);
    EVP_CIPHER_CTX_free(ctx);
    ZERO_FREE(buf, bufsize);
    memset(key, 0, sizeof(key));
    memset(iv, 0, sizeof(iv));
    if (win) { delwin(win); }
    endwin();

    return ((cancelled == 0) && (written == size));
}

// CSV wipe log entry
void log_wipe(const char *dev, int result) {
    FILE *fp = fopen(LOG_FILE, "a"); time_t now = time(NULL);
    if (fp) {
        fprintf(fp, "%ld,%s,%s\n", now, dev, result ? "SUCCESS": "FAIL");
        fclose(fp);
    }
}

int main(int argc, char *argv[]) {
    if (geteuid() != 0) { fprintf(stderr,"Run as root.\n"); return 1; }
    if (argc != 2) { printf("Usage: %s /dev/sdX\n", argv[0]); return 1; }
    const char *dev = argv[1];

    // Device existence and NOT mounted check
    if (access(dev, F_OK) != 0) { printf("Device not found.\n"); return 1; }
    char mount_chk[256]; snprintf(mount_chk, sizeof(mount_chk),
        "lsblk -no MOUNTPOINT %s | grep -v '^$'", dev);
    if (system(mount_chk) == 0) {
        printf("Device %s appears mounted! Unmount first.\n", dev); return 1;
    }

    int ssd = is_ssd(dev);
    printf("Selected device: %s (%s)\n", dev, ssd ? "SSD" : "HDD");
    printf("About to wipe %s. Press y to continue: ", dev);
    int c = getchar(); if (c != 'y' && c != 'Y') return 0;

    int result = secure_wipe(dev);
    log_wipe(dev, result);
    printf("Wipe result: %s\n", result ? "SUCCESS" : "FAIL/ABORT");
    return result ? 0 : 2;
}

