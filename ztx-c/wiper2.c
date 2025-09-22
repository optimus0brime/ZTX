#include <ncurses.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <sys/types.h>

#define MAX_INPUT   128
#define MAX_NAME    128
#define MAX_DEVICES 64
#define LOG_FILE    "wipe_log.csv"

extern int ESCDELAY;

typedef struct {
    char name[MAX_NAME];
    char size[MAX_NAME];
    char model[MAX_NAME];
} Device;

// ------- Partition filter (supports NVMe) -------
int is_partition_name(const char *name) {
    int len = strlen(name);
    if (len == 0) return 0;
    char *p = strstr(name,"p");
    if(p && isdigit(*(p+1))) return 1; // nvme0n1p1
    if(name[0]=='s' && isdigit(name[len-1])) return 1; // sda1
    return 0;
}

// ------- Device Loader -------
int load_devices(Device devices[]) {
    FILE *fp = popen("lsblk -dn -o NAME,SIZE,MODEL", "r");
    if (!fp) return 0;
    char line[512]; int count=0;
    while(fgets(line,sizeof(line),fp) && count<MAX_DEVICES){
        char name[MAX_NAME],size[MAX_NAME],model[MAX_NAME]="";
        int f = sscanf(line,"%s %s %[^\n]",name,size,model);
        if(f>=2){
            if(is_partition_name(name)) continue;
            snprintf(devices[count].name,MAX_NAME,"%s",name);
            snprintf(devices[count].size,MAX_NAME,"%s",size);
            snprintf(devices[count].model,MAX_NAME,"%s",model);
            count++;
        }
    }
    pclose(fp); return count;
}

// ------- Device Selection TUI -------
char *select_device() {
    static char selected[256];
    Device devices[MAX_DEVICES]; int n = load_devices(devices);
    if(n==0){ printf("No devices!\n"); return NULL; }

    initscr(); ESCDELAY=25; noecho(); cbreak(); keypad(stdscr,1);
    int highlight=0,ch;
    while(1){
        clear(); box(stdscr,0,0);
        mvprintw(0,2," Select Device (Enter=Select, Q/ESC=Exit) ");
        for(int i=0;i<n;i++){
            if(i==highlight) attron(A_REVERSE);
            mvprintw(i+2,2,"/dev/%s (%s - %s)",devices[i].name,devices[i].size,devices[i].model);
            if(i==highlight) attroff(A_REVERSE);
        }
        refresh();
        ch=getch();
        if(ch==KEY_UP) highlight=(highlight==0)?n-1:highlight-1;
        else if(ch==KEY_DOWN) highlight=(highlight==n-1)?0:highlight+1;
        else if(ch=='q'||ch=='Q'||ch==27){ endwin(); return NULL; }
        else if(ch=='\n'){
            snprintf(selected,sizeof(selected),"/dev/%s",devices[highlight].name);
            endwin(); return selected;
        }
    }
}

// ------- Device Info Preview -------
int preview_device(const char *dev) {
    char cmd[256], buf[256];
    initscr(); box(stdscr,0,0);
    mvprintw(1,2," Device Info ");
    snprintf(cmd,sizeof(cmd),"lsblk -d -o NAME,SIZE,MODEL,ROTA %s",dev);
    FILE *fp=popen(cmd,"r");
    int line=3; while(fgets(buf,sizeof(buf),fp)){ mvprintw(line++,2,"%s",buf); }
    pclose(fp);
    mvprintw(line+1,2,"Press Enter to continue or ESC to cancel");
    refresh(); int ch=getch(); endwin();
    if(ch==27) return 0; return 1;
}

// ------- Masked Password -------
void get_hidden(WINDOW *win,int y,int x,char *buf,int sz){
    int ch,idx=0; while((ch=wgetch(win))!='\n'&&idx<sz-1){
        if((ch==KEY_BACKSPACE||ch==127)&&idx>0){ idx--; mvwprintw(win,y,x+idx," "); wmove(win,y,x+idx);}
        else if(ch>=32&&ch<=126){ buf[idx++]=ch; mvwprintw(win,y,x+idx-1,"*");}
        wrefresh(win);
    } buf[idx]='\0';
}

// ------- Confirmation Screen -------
int confirm_wipe(const char *dev,char *user,char *temp,char *pass){
    initscr(); ESCDELAY=25; noecho(); cbreak(); curs_set(1);
    int w=COLS-4,h=14; WINDOW *win=newwin(h,w,(LINES-h)/2,2); box(win,0,0);
    mvwprintw(win,0,2," Confirm Wipe "); mvwprintw(win,1,2,"Device: %s",dev);
    mvwprintw(win,3,2,"User Name: "); mvwprintw(win,4,2,"Temp Name: "); mvwprintw(win,5,2,"Password: ");
    mvwprintw(win,7,2,"Type WIPE to confirm: "); mvwprintw(win,9,2,"ESC=Cancel");
    wrefresh(win);
    int ch=wgetch(win); if(ch==27){ endwin(); return 0;} ungetch(ch);
    echo(); mvwgetnstr(win,3,14,user,MAX_INPUT-1); mvwgetnstr(win,4,14,temp,MAX_INPUT-1);
    noecho(); get_hidden(win,5,12,pass,MAX_INPUT);
    char buf[MAX_INPUT]; echo(); mvwgetnstr(win,7,23,buf,MAX_INPUT-1);
    endwin();
    if(strcmp(buf,"WIPE")!=0) return 0;
    return 1;
}

// ------- Progress Bar -------
void progress_bar(WINDOW *win,int y,int x,double frac,int w){
    int f=(int)(frac*w); mvwprintw(win,y,x,"[");
    for(int i=0;i<w;i++) waddch(win,(i<f)?'#':'-'); wprintw(win,"] %3.1f%%",frac*100);
}

// ------- Device Size -------
long dev_size_bytes(const char *dev){
    char cmd[256],buf[256]; snprintf(cmd,sizeof(cmd),"lsblk -nb -o SIZE %s",dev);
    FILE *fp=popen(cmd,"r"); if(!fp) return 0; if(fgets(buf,sizeof(buf),fp)){pclose(fp);return atol(buf);} pclose(fp); return 0;
}

// ------- Detect SSD -------
int is_ssd(const char *device) {
    char cmd[256], buf[64];
    snprintf(cmd,sizeof(cmd),"lsblk -dn -o ROTA %s",device);
    FILE *fp=popen(cmd,"r"); if(!fp) return 0;
    if(fgets(buf,sizeof(buf),fp)){pclose(fp);return (atoi(buf)==0);} pclose(fp); return 0;
}

// ------- HDD Wipe (2-pass dd) -------
int wipe_device(const char *dev){
    initscr(); noecho(); cbreak(); curs_set(0); keypad(stdscr,1);
    int w=COLS-4,h=14; 
    WINDOW *win=newwin(h,w,(LINES-h)/2,2);

    const char *src[2]={"/dev/zero","/dev/urandom"}; 
    long size=dev_size_bytes(dev);

    for(int p=0;p<2;p++){
        char cmd[512],buf[256]; 
        snprintf(cmd,sizeof(cmd),
                 "dd if=%s of=%s bs=64M status=progress 2>&1",
                 src[p],dev);

        FILE *fp=popen(cmd,"r"); 
        if(!fp) return 0;

        long copied=0; 
        time_t start=time(NULL); 
        int ch;

        while(fgets(buf,sizeof(buf),fp)){
            sscanf(buf,"%ld",&copied);
            double frac=size?(double)copied/size:0; 
            if(frac>1) frac=1;

            time_t now=time(NULL);
            double elapsed=difftime(now,start);
            double speed=(elapsed>0)?(copied/1048576.0)/elapsed:0; // in MiB/s
            double eta=(speed>0)?( (size-copied)/1048576.0 / speed ):0; // seconds

            werase(win); box(win,0,0);
            mvwprintw(win,0,2," Wiping Device ");
            mvwprintw(win,1,2,"Device: %s",dev);
            mvwprintw(win,2,2,"Pass %d/2: %s",p+1,(p==0)?"Zeroes":"Random");

            // progress bar
            progress_bar(win,5,2,frac,w-10);

            // display human-friendly stats
            mvwprintw(win,7,2,"Copied: %.2f GiB / %.2f GiB",
                      copied/1073741824.0, size/1073741824.0);
            mvwprintw(win,8,2,"Speed:  %.1f MiB/s", speed);
            mvwprintw(win,9,2,"ETA:    %.0f sec", eta);
            mvwprintw(win,11,2,"Q/ESC=Abort");

            wrefresh(win);
            nodelay(win,1); ch=wgetch(win);
            if(ch=='q'||ch=='Q'||ch==27){ pclose(fp); endwin(); return 0;}
        }
        pclose(fp);
    }
    endwin();
    return 1;
}


// ------- SSD Handler -------
int handle_ssd(const char *dev){
    initscr(); box(stdscr,0,0);
    mvprintw(1,2,"SSD Detected: %s",dev);
    mvprintw(3,2,"Recommended: Secure Erase via controller");
    mvprintw(5,2,"Enter = dd overwrite (not guaranteed)");
    mvprintw(6,2,"S     = hdparm/nvme secure erase");
    mvprintw(7,2,"Q/ESC = cancel"); refresh(); int ch=getch(); endwin();
    if(ch=='q'||ch=='Q'||ch==27) return 0;
    if(ch=='s'||ch=='S'){
        char cmd[512]; if(strstr(dev,"nvme")) snprintf(cmd,sizeof(cmd),"nvme format %s --ses=1",dev);
        else snprintf(cmd,sizeof(cmd),"hdparm --user-master u --security-set-pass p %s && hdparm --user-master u --security-erase p %s",dev,dev);
        return system(cmd)==0?1:0;
    }
    return 2; // dd fallback
}

// ------- Logger -------
void log_wipe(const char *u,const char *t,const char *d,int s){
    FILE *fp=fopen(LOG_FILE,"a"); if(!fp)return; time_t now=time(NULL);
    fprintf(fp,"%ld,%s,%s,%s,%s\n",now,u,t,d,s?"SUCCESS":"CANCELLED"); fclose(fp);
}

// ------- Summary Screen -------
void summary_screen(const char *user,const char *temp,const char *dev,int res){
    initscr(); box(stdscr,0,0);
    mvprintw(1,2,"Wipe Summary");
    mvprintw(3,2,"Operator: %s",user);
    mvprintw(4,2,"Temp Name: %s",temp);
    mvprintw(5,2,"Device: %s",dev);
    mvprintw(6,2,"Result: %s",res?"SUCCESS":"CANCELLED");
    mvprintw(8,2,"Press any key to exit.");
    refresh(); getch(); endwin();
}

// ------- Main -------
int main(){
    if(geteuid()!=0){ initscr(); box(stdscr,0,0); mvprintw(2,2,"Root required! Run with sudo."); refresh(); getch(); endwin(); return 1;}
    char *dev=select_device(); if(!dev) return 0;
    if(!preview_device(dev)) return 0;  // new info screen

    char user[MAX_INPUT],temp[MAX_INPUT],pass[MAX_INPUT];
    if(!confirm_wipe(dev,user,temp,pass)) return 0;

    int result=0;
    if(is_ssd(dev)){
        int a=handle_ssd(dev);
        if(a==0){ printf("Cancelled.\n"); return 0;}
        else if(a==1){ result=1;}
        else if(a==2){ result=wipe_device(dev); }
    } else {
        result=wipe_device(dev);
    }

    log_wipe(user,temp,dev,result);
    summary_screen(user,temp,dev,result);
    return 0;
}

