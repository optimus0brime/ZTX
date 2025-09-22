#include <ncurses.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <openssl/evp.h>

#define WIPER_MAX_INPUT    128
#define MAX_NAME     128
#define MAX_DEVICES  64
#define LOG_FILE     "wipe_log.csv"
#define WIPER_BUF_GIB 1 // RAM usage (1–4), 1=1GiB, 4=4GiB, etc

typedef struct {
    char name[MAX_NAME];
    char size[MAX_NAME];
    char model[MAX_NAME];
} Device;

extern int ESCDELAY;

// ------ Partition detection ------
int is_partition_name(const char *name) {
    int len = strlen(name);
    if (len == 0) return 0;
    char *p = strstr(name, "p");
    if (p && isdigit(*(p + 1))) return 1; // nvme0n1p1
    if (name[0] == 's' && isdigit(name[len - 1])) return 1; // sda1
    return 0;
}

// ------ Detect device list ------
int load_devices(Device devices[]) {
    FILE *fp = popen("lsblk -dn -o NAME,SIZE,MODEL", "r");
    if (!fp) return 0;
    char line[512]; int count = 0;
    while (fgets(line, sizeof(line), fp) && count < MAX_DEVICES) {
        char name[MAX_NAME], size[MAX_NAME], model[MAX_NAME]="";
        int f = sscanf(line, "%s %s %[^\n]", name, size, model);
        if (f >= 2) {
            if(is_partition_name(name)) continue;
            snprintf(devices[count].name, MAX_NAME, "%s", name);
            snprintf(devices[count].size, MAX_NAME, "%s", size);
            snprintf(devices[count].model, MAX_NAME, "%s", model);
            count++;
        }
    }
    pclose(fp); return count;
}

// ------ Device selector ------
char *select_device() {
    static char selected[256];
    Device devices[MAX_DEVICES];
    int n = load_devices(devices);
    if (n == 0) { printf("No block devices found!\n"); return NULL; }

    initscr(); ESCDELAY=25; noecho(); cbreak(); keypad(stdscr, 1);
    int highlight=0, ch;
    while(1) {
        clear(); box(stdscr,0,0);
        mvprintw(0,2," Select Device (Enter=Select, Q/ESC=Exit) ");
        for(int i=0; i<n; i++) {
            if(i==highlight) attron(A_REVERSE);
            mvprintw(i+2,2,"/dev/%s (%s - %s)", devices[i].name, devices[i].size, devices[i].model);
            if(i==highlight) attroff(A_REVERSE);
        }
        refresh(); ch=getch();
        if(ch==KEY_UP) highlight=(highlight==0)?n-1:highlight-1;
        else if(ch==KEY_DOWN) highlight=(highlight==n-1)?0:highlight+1;
        else if(ch=='q'||ch=='Q'||ch==27){ endwin(); return NULL; }
        else if(ch=='\n') { snprintf(selected, sizeof(selected), "/dev/%s", devices[highlight].name); endwin(); return selected; }
    }
}

// ------ Device preview/info ------
int preview_device(const char *dev) {
    initscr(); box(stdscr,0,0);
    mvprintw(1,2," Device Info "); refresh();
    char cmd[256], buf[256];
    snprintf(cmd, sizeof(cmd), "lsblk -d -o NAME,SIZE,MODEL,ROTA %s", dev);
    FILE *fp = popen(cmd, "r");
    int line=3; while(fgets(buf,sizeof(buf),fp)){ mvprintw(line++,2,"%s",buf); }
    pclose(fp);
    mvprintw(line+1,2,"Press Enter to continue or ESC to cancel");
    refresh(); int ch=getch(); endwin();
    if (ch==27) return 0; return 1;
}

// ------ Password input, confirmation ------
void get_hidden(WINDOW *win, int y, int x, char *buf, int sz) {
    int ch, idx=0;
    while((ch=wgetch(win))!='\n'&&idx<sz-1){
        if((ch==KEY_BACKSPACE||ch==127)&&idx>0){ idx--; mvwprintw(win,y,x+idx," "); wmove(win,y,x+idx);}
        else if(ch>=32&&ch<=126){ buf[idx++]=ch; mvwprintw(win,y,x+idx-1,"*");}
        wrefresh(win);
    }
    buf[idx]='\0';
}

int confirm_wipe(const char *dev, char *user, char *temp, char *pass) {
    initscr(); ESCDELAY=25; noecho(); cbreak(); curs_set(1);
    int w=COLS-4,h=14; WINDOW *win=newwin(h,w,(LINES-h)/2,2); box(win,0,0); keypad(win,1);
    mvwprintw(win,0,2," Confirm Wipe "); mvwprintw(win,1,2,"Device: %s",dev);
    mvwprintw(win,3,2,"User Name: "); mvwprintw(win,4,2,"Temp Name: "); mvwprintw(win,5,2,"Password: ");
    mvwprintw(win,7,2,"Type WIPE to confirm: "); mvwprintw(win,9,2,"ESC=Cancel"); wrefresh(win);
    int ch=wgetch(win); if(ch==27){ endwin(); return 0;} ungetch(ch);
    echo(); mvwgetnstr(win,3,14,user,WIPER_MAX_INPUT-1); mvwgetnstr(win,4,14,temp,WIPER_MAX_INPUT-1);
    noecho(); get_hidden(win,5,12,pass,WIPER_MAX_INPUT);
    char buf[WIPER_MAX_INPUT]; echo(); mvwgetnstr(win,7,23,buf,WIPER_MAX_INPUT-1);
    endwin();
    if(strcmp(buf,"WIPE")!=0) return 0;
    return 1;
}

// ------ Helpers ------
void progress_bar(WINDOW *win,int y,int x,double frac,int w){
    int f=(int)(frac*w);
    mvwprintw(win,y,x,"[");
    for(int i=0;i<w;i++) waddch(win,(i<f)?'#':'-');
    wprintw(win,"] %3.1f%%",frac*100);
}

long dev_size_bytes(const char *dev) {
    char cmd[256], buf[256];
    snprintf(cmd, sizeof(cmd), "lsblk -nb -o SIZE %s", dev);
    FILE *fp=popen(cmd,"r"); if(!fp) return 0;
    if(fgets(buf,sizeof(buf),fp)){ pclose(fp); return atol(buf);}
    pclose(fp); return 0;
}

// ------ Detect SSD ------
int is_ssd(const char *device) {
    char cmd[256],buf[64];
    snprintf(cmd,sizeof(cmd),"lsblk -dn -o ROTA %s",device);
    FILE *fp=popen(cmd,"r"); if(!fp) return 0;
    if(fgets(buf,sizeof(buf),fp)){ pclose(fp); return (atoi(buf)==0); }
    pclose(fp); return 0;
}

// ------ Native single-pass AES-CTR random overwrite (HDD) ------
int wipe_device(const char *dev) {
    initscr(); noecho(); cbreak(); curs_set(0); keypad(stdscr,1);
    int w=COLS-4,h=16; WINDOW *win=newwin(h,w,(LINES-h)/2,2);

    long size = dev_size_bytes(dev);
    if(size <= 0) size = 1;
    size_t buf_size = WIPER_BUF_GIB * (size_t)1073741824;
    if (buf_size > size) buf_size = size;
    unsigned char *buf = NULL;
    if (posix_memalign((void**)&buf, 4096, buf_size)) {
        endwin(); printf("Buffer alloc failed\n"); return 0;
    }

    // Create key, IV, AES context
    unsigned char key[32], iv[16];
    FILE *rnd = fopen("/dev/urandom", "rb");
    fread(key, 1, sizeof(key), rnd); fread(iv, 1, sizeof(iv), rnd); fclose(rnd);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv);

    int fd = open(dev, O_WRONLY | O_SYNC);
    if (fd < 0) {
        endwin(); perror("open"); EVP_CIPHER_CTX_free(ctx); free(buf); return 0;
    }

    long written = 0;
    time_t start = time(NULL);

    while (written < size) {
        size_t to_write = (size - written > buf_size) ? buf_size : size - written;
        memset(buf, 0, to_write); // zeroes plaintext
        int out_len = 0;
        // AES-CTR encrypt zeros -> random
        EVP_EncryptUpdate(ctx, buf, &out_len, buf, to_write);

        ssize_t n = write(fd, buf, to_write); // Use O_DIRECT/O_SYNC to bypass cache (can fully erase)
        if (n <= 0) { perror("write"); break; }
        written += n;

        double frac = (double)written/size;
        time_t now = time(NULL);
        double elapsed = difftime(now,start);
        double speed = (elapsed>0)?(written/1048576.0)/elapsed:0;
        double eta = (speed>0)?((size-written)/1048576.0)/speed:0;

        werase(win); box(win,0,0);
        mvwprintw(win,0,2," Secure HDD Wipe: AES-CTR random (1 pass)");
        mvwprintw(win,1,2,"Device: %s", dev);
        progress_bar(win,5,2,frac,w-10);
        mvwprintw(win,7,2,"Written: %.2f / %.2f GiB", written/1073741824.0, size/1073741824.0);
        mvwprintw(win,8,2,"Speed: %.1f MiB/s", speed);
        mvwprintw(win,9,2,"ETA:   %.0f sec", eta);
        mvwprintw(win,11,2,"Q/ESC=Abort");
        wrefresh(win);

        nodelay(win,1); int ch=wgetch(win);
        if(ch=='q'||ch=='Q'||ch==27) { break; }
    }
    close(fd);
    EVP_CIPHER_CTX_free(ctx);
    free(buf);
    endwin();
    return (written >= size);
}

// ------ SSD wipe helper ------
int handle_ssd(const char *dev) {
    initscr(); box(stdscr,0,0);
    mvwprintw(stdscr,1,2,"SSD Detected: %s",dev);
    mvwprintw(stdscr,2,2,"Recommended: Secure Erase (hdparm/nvme/blkdiscard)");
    mvwprintw(stdscr,3,2,"1 = hdparm (SATA SSD)");
    mvwprintw(stdscr,4,2,"2 = nvme format (NVMe SSD)");
    mvwprintw(stdscr,5,2,"3 = blkdiscard (if supported)");
    mvwprintw(stdscr,6,2,"F = Fallback AES-CTR overwrite (NOT guaranteed secure)");
    mvwprintw(stdscr,7,2,"Q/ESC = Cancel");
    refresh(); int ch=getch(); endwin();

    if(ch=='q'||ch=='Q'||ch==27) return 0;
    if(ch=='f'||ch=='F') return 2; // fallback native wipe

    if(ch=='1') {
        char cmd[512];
        snprintf(cmd,sizeof(cmd),
            "hdparm --user-master u --security-set-pass p %s && hdparm --user-master u --security-erase p %s",dev,dev);
        return (system(cmd)==0)?1:0;
    }
    if(ch=='2') {
        char cmd[512];
        snprintf(cmd,sizeof(cmd),"nvme format %s --ses=1",dev);
        return (system(cmd)==0)?1:0;
    }
    if(ch=='3') {
        char cmd[512];
        snprintf(cmd,sizeof(cmd),"blkdiscard %s",dev);
        return (system(cmd)==0)?1:0;
    }

    return 0;
}

// ------ Logging, summary ------
void log_wipe(const char *u,const char *t,const char *d,int s){
    FILE *fp=fopen(LOG_FILE,"a"); if(!fp)return; time_t now=time(NULL);
    fprintf(fp,"%ld,%s,%s,%s,%s\n",now,u,t,d,s?"SUCCESS":"CANCELLED"); fclose(fp);
}

void summary_screen(const char *user,const char *temp,const char *dev,int res){
    initscr(); box(stdscr,0,0);
    mvwprintw(stdscr,1,2,"Wipe Summary");
    mvwprintw(stdscr,3,2,"Operator: %s",user);
    mvwprintw(stdscr,4,2,"Temp Name: %s",temp);
    mvwprintw(stdscr,5,2,"Device: %s",dev);
    mvwprintw(stdscr,6,2,"Result: %s",res?"SUCCESS":"CANCELLED");
    mvwprintw(stdscr,8,2,"Press any key to exit.");
    refresh(); getch(); endwin();
}

int main() {
    if(geteuid()!=0){ initscr(); box(stdscr,0,0);
        mvwprintw(stdscr,2,2,"Root required! Run with sudo."); refresh(); getch(); endwin(); return 1; }

    char *dev=select_device(); if(!dev) return 0;
    if(!preview_device(dev)) return 0;

    char user[WIPER_MAX_INPUT],temp[WIPER_MAX_INPUT],pass[WIPER_MAX_INPUT];
    if(!confirm_wipe(dev,user,temp,pass)) return 0;

    int result=0;
    if(is_ssd(dev)){
        int a=handle_ssd(dev);
        if(a==0){
            initscr(); box(stdscr,0,0);
            mvwprintw(stdscr,2,2,"Wipe Cancelled or Secure Erase Failed!");
            mvwprintw(stdscr,4,2,"Press any key."); refresh(); getch(); endwin(); 
            return 0;
        }
        else if(a==1){ result=1; }
        else if(a==2){
            initscr(); box(stdscr,0,0);
            mvwprintw(stdscr,2,2,"Fallback: Software overwrite on SSD is NOT guaranteed secure!!");
            mvwprintw(stdscr,3,2,"Press any key to proceed or ESC to cancel...");
            refresh(); int ch=getch(); endwin();
            if(ch==27) return 0;
            result=wipe_device(dev);
        }
    } else {
        result=wipe_device(dev);
    }

    log_wipe(user,temp,dev,result);
    summary_screen(user,temp,dev,result);
    return 0;
}

