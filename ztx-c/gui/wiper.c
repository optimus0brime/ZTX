#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_DEVICES   64
#define MAX_NAME      64
#define MAX_LINES     8192
#define MAX_LINE_LEN  512

typedef struct {
    char name[MAX_NAME];
    char type[16]; // e.g., "disk", "part"
    char bus[16];  // e.g., "ata", "nvme"
} Device;

int load_devices(Device devices[]) {
    FILE *fp = popen("lsblk -d -o NAME,TYPE,TRAN | grep disk", "r");
    if (!fp) return 0;
    char line[256];
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < MAX_DEVICES) {
        char name[MAX_NAME], type[16], bus[16];
        if (sscanf(line, "%s %s %s", name, type, bus) >= 2) {
            snprintf(devices[count].name, MAX_NAME, "%s", name);
            snprintf(devices[count].type, sizeof(devices[count].type), "%s", type);
            snprintf(devices[count].bus, sizeof(devices[count].bus), "%s", bus[0] ? bus : "unknown");
            count++;
        }
    }
    pclose(fp);
    return count;
}

int run_cmd(const char *cmd, char lines[MAX_LINES][MAX_LINE_LEN], int count) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return count;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), fp) && count < MAX_LINES) {
        buffer[strcspn(buffer, "\n")] = 0;
        snprintf(lines[count++], MAX_LINE_LEN, "%s", buffer);
    }
    pclose(fp);
    return count;
}

int add_header(const char *title, char lines[MAX_LINES][MAX_LINE_LEN], int count) {
    snprintf(lines[count++], MAX_LINE_LEN, "=== %s ===", title);
    return count;
}

int get_device_details(const char *devname, char lines[MAX_LINES][MAX_LINE_LEN]) {
    int count = 0;
    char cmd[1024];

    count = add_header("Device Info", lines, count);
    snprintf(cmd, sizeof(cmd), "lsblk -o NAME,SIZE,TYPE,TRAN,MODEL,VENDOR -P /dev/%s", devname);
    count = run_cmd(cmd, lines, count);
    strcpy(lines[count++], "");

    count = add_header("blkid Info", lines, count);
    snprintf(cmd, sizeof(cmd), "blkid /dev/%s", devname);
    count = run_cmd(cmd, lines, count);
    strcpy(lines[count++], "");

    count = add_header("Wipe Support", lines, count);
    snprintf(cmd, sizeof(cmd), "hdparm -I /dev/%s 2>/dev/null | grep -i 'security.*supported'", devname);
    count = run_cmd(cmd, lines, count);
    snprintf(cmd, sizeof(cmd), "nvme id-ctrl /dev/%s 2>/dev/null | grep -i format", devname);
    count = run_cmd(cmd, lines, count);
    strcpy(lines[count++], "");

    return count;
}

int confirm_wipe(WINDOW *win, const char *devname) {
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " Confirm Wipe ");
    mvwprintw(win, 2, 2, "WARNING: Wiping /dev/%s will destroy all data!", devname);
    mvwprintw(win, 3, 2, "Type 'WIPE' to confirm, or press 'q' to cancel:");
    wrefresh(win);

    char input[16] = {0};
    int pos = 0;
    int ch;
    echo();
    while ((ch = getch()) != '\n') {
        if (ch == 'q' || ch == 'Q') return 0;
        if (pos < 15 && isalnum(ch)) {
            input[pos++] = ch;
            mvwprintw(win, 4, 2, "%s", input);
            wrefresh(win);
        }
    }
    noecho();
    input[pos] = '\0';
    return strcmp(input, "WIPE") == 0;
}

int wipe_device(const char *devname, const char *bus, char lines[MAX_LINES][MAX_LINE_LEN]) {
    int count = 0;
    char cmd[1024];

    count = add_header("Wipe Operation", lines, count);
    if (strcmp(bus, "ata") == 0) {
        snprintf(cmd, sizeof(cmd), "hdparm --user-master u --security-set-pass p /dev/%s 2>&1", devname);
        count = run_cmd(cmd, lines, count);
        snprintf(cmd, sizeof(cmd), "hdparm --user-master u --security-erase p /dev/%s 2>&1", devname);
        count = run_cmd(cmd, lines, count);
    } else if (strcmp(bus, "nvme") == 0) {
        snprintf(cmd, sizeof(cmd), "nvme format /dev/%s --ses=1 2>&1", devname);
        count = run_cmd(cmd, lines, count);
    } else {
        snprintf(lines[count++], MAX_LINE_LEN, "Unsupported device type: %s", bus);
    }
    return count;
}

int main() {
    Device devices[MAX_DEVICES];
    int device_count = load_devices(devices);
    if (device_count == 0) {
        printf("No storage devices found!\n");
        return 1;
    }

    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);

    int height = LINES - 2;
    int width = COLS / 2 - 2;

    WINDOW *dev_win = newwin(height, width, 1, 1);
    WINDOW *detail_win = newwin(height, width, 1, COLS / 2);

    int highlight = 0, scroll_offset = 0;
    char detail_lines[MAX_LINES][MAX_LINE_LEN];
    int total_detail_lines = get_device_details(devices[highlight].name, detail_lines);

    while (1) {
        werase(dev_win);
        box(dev_win, 0, 0);
        mvwprintw(dev_win, 0, 2, " Devices ");
        for (int i = 0; i < device_count; i++) {
            if (i == highlight) wattron(dev_win, A_REVERSE);
            mvwprintw(dev_win, i+1, 2, "%s (%s)", devices[i].name, devices[i].bus);
            wattroff(dev_win, A_REVERSE);
        }
        wrefresh(dev_win);

        werase(detail_win);
        box(detail_win, 0, 0);
        mvwprintw(detail_win, 0, 2, " Details ");

        if (scroll_offset > total_detail_lines - (height - 2))
            scroll_offset = (total_detail_lines > height - 2) ? total_detail_lines - (height - 2) : 0;
        if (scroll_offset < 0) scroll_offset = 0;

        int line_no = 1;
        int max_line_width = width - 3;

        for (int i = scroll_offset; i < total_detail_lines && line_no < height - 1; i++) {
            char temp[MAX_LINE_LEN];
            strncpy(temp, detail_lines[i], max_line_width);
            temp[max_line_width] = '\0';
            if (strncmp(temp, "===", 3) == 0) {
                wattron(detail_win, A_BOLD | A_UNDERLINE);
                mvwprintw(detail_win, line_no++, 1, "%s", temp);
                wattroff(detail_win, A_BOLD | A_UNDERLINE);
            } else if (strcmp(temp, "---") == 0) {
                wattron(detail_win, COLOR_PAIR(1) | A_BOLD);
                mvwprintw(detail_win, line_no++, 1, "%s", temp);
                wattroff(detail_win, COLOR_PAIR(1) | A_BOLD);
            } else {
                mvwprintw(detail_win, line_no++, 1, "%s", temp);
            }
        }

        mvwprintw(detail_win, height - 1, 2, "[%d/%d] Press 'w' to wipe, 'q' to quit", scroll_offset+1, total_detail_lines);
        wrefresh(detail_win);

        int ch = getch();
        switch (ch) {
            case KEY_UP:
                highlight = (highlight == 0) ? device_count-1 : highlight-1;
                scroll_offset = 0;
                total_detail_lines = get_device_details(devices[highlight].name, detail_lines);
                break;
            case KEY_DOWN:
                highlight = (highlight == device_count-1) ? 0 : highlight+1;
                scroll_offset = 0;
                total_detail_lines = get_device_details(devices[highlight].name, detail_lines);
                break;
            case KEY_NPAGE: scroll_offset += height - 3; break;
            case KEY_PPAGE: scroll_offset -= height - 3; break;
            case 'j': scroll_offset++; break;
            case 'k': scroll_offset--; break;
            case 'w': case 'W':
                if (confirm_wipe(detail_win, devices[highlight].name)) {
                    total_detail_lines = wipe_device(devices[highlight].name, devices[highlight].bus, detail_lines);
                } else {
                    total_detail_lines = get_device_details(devices[highlight].name, detail_lines);
                }
                scroll_offset = 0;
                break;
            case 'q': case 'Q': endwin(); return 0;
        }
    }
    endwin();
    return 0;
}
