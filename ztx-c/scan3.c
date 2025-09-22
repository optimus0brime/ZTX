#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEVICES   64
#define MAX_NAME      64
#define MAX_LINES     8192
#define MAX_LINE_LEN  512

typedef struct {
    char name[MAX_NAME];
} Device;

int load_devices(Device devices[]) {
    FILE *fp = fopen("/proc/partitions", "r");
    if (!fp) return 0;
    char line[256];
    int count = 0;

    // Skip headers
    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);

    while (fgets(line, sizeof(line), fp) && count < MAX_DEVICES) {
        int major, minor;
        long blocks;
        char name[MAX_NAME];
        if (sscanf(line, "%d %d %ld %s", &major, &minor, &blocks, name) == 4) {
            snprintf(devices[count].name, MAX_NAME, "%s", name);
            count++;
        }
    }
    fclose(fp);
    return count;
}

// Helper: run a shell command, dump lines into buffer
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

// Add section header
int add_header(const char *title, char lines[MAX_LINES][MAX_LINE_LEN], int count) {
    snprintf(lines[count++], MAX_LINE_LEN, "=== %s ===", title);
    return count;
}

// Gather full device details into lines[] array
int get_device_details(const char *devname, char lines[MAX_LINES][MAX_LINE_LEN]) {
    int count = 0;
    char cmd[1024], buffer[2048];

    // 1. General Info (lsblk -P parsed vertically)
    count = add_header("General Info", lines, count);
    snprintf(cmd, sizeof(cmd),
        "lsblk -o NAME,SIZE,MODEL,VENDOR,SERIAL,ROTA,TYPE,FSTYPE,MOUNTPOINT,"
        "UUID,LABEL,PARTUUID,PARTLABEL,STATE,OWNER,GROUP,MODE,FSAVAIL,FSSIZE,"
        "FSUSED,HOTPLUG,TRAN,ALIGNMENT -P /dev/%s", devname);

    FILE *fp = popen(cmd, "r");
    if (fp) {
        int firstNameSeen = 0;
        while (fgets(buffer, sizeof(buffer), fp)) {
            buffer[strcspn(buffer, "\n")] = 0;
            char *tok = strtok(buffer, " ");
            while (tok && count < MAX_LINES) {
                char key[64], val[448];
                if (sscanf(tok, "%63[^=]=\"%447[^\"]\"", key, val) == 2) {
                    if (strlen(val) == 0) strcpy(val, "-");
                    if (strcmp(key, "NAME") == 0) {
                        if (firstNameSeen) { // add separator between entries
                            strcpy(lines[count++], "---");
                        }
                        firstNameSeen = 1;
                    }
                    snprintf(lines[count++], MAX_LINE_LEN, "%-12s : %s", key, val);
                }
                tok = strtok(NULL, " ");
            }
        }
        pclose(fp);
    }
    strcpy(lines[count++], "");

    // 2. Kernel sysfs
    count = add_header("Kernel sysfs", lines, count);
    char sys_path[256];
    FILE *sysfp;
    snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/queue/logical_block_size", devname);
    sysfp = fopen(sys_path, "r"); if (sysfp) {int v; fscanf(sysfp, "%d",&v); fclose(sysfp);
        snprintf(lines[count++], MAX_LINE_LEN, "LOGICAL_BLOCK_SIZE : %d", v);}
    snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/queue/physical_block_size", devname);
    sysfp = fopen(sys_path, "r"); if (sysfp) {int v; fscanf(sysfp, "%d",&v); fclose(sysfp);
        snprintf(lines[count++], MAX_LINE_LEN, "PHYSICAL_BLOCK_SIZE: %d", v);}
    snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/queue/rotational", devname);
    sysfp = fopen(sys_path, "r"); if (sysfp) {int v; fscanf(sysfp, "%d",&v); fclose(sysfp);
        snprintf(lines[count++], MAX_LINE_LEN, "ROTATIONAL         : %s", v ? "HDD" : "SSD");}
    snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/queue/nr_requests", devname);
    sysfp = fopen(sys_path, "r"); if (sysfp) {int v; fscanf(sysfp, "%d",&v); fclose(sysfp);
        snprintf(lines[count++], MAX_LINE_LEN, "QUEUE_DEPTH        : %d", v);}
    strcpy(lines[count++], "");

    // 3. udevadm Properties
    count = add_header("udevadm Properties", lines, count);
    snprintf(cmd, sizeof(cmd),
             "udevadm info --query=property --name=/dev/%s | "
             "grep -E 'ID_MODEL=|ID_VENDOR=|ID_SERIAL=|ID_REVISION=|ID_TYPE=|ID_PATH=|ID_BUS='",
             devname);
    count = run_cmd(cmd, lines, count);
    strcpy(lines[count++], "");

    // 4. SMART Health
    count = add_header("SMART Health", lines, count);
    snprintf(cmd, sizeof(cmd), "smartctl -i -H -A /dev/%s 2>/dev/null", devname);
    count = run_cmd(cmd, lines, count);
    strcpy(lines[count++], "");

    // 5. hdparm Capabilities
    count = add_header("hdparm Capabilities", lines, count);
    snprintf(cmd, sizeof(cmd), "hdparm -I /dev/%s 2>/dev/null | head -40", devname);
    count = run_cmd(cmd, lines, count);

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

    // Color setup: yellow separators
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);

    int height = LINES - 2;
    int width  = COLS / 2 - 2;

    WINDOW *dev_win    = newwin(height, width, 1, 1);
    WINDOW *detail_win = newwin(height, width, 1, COLS / 2);

    int highlight = 0;
    int ch, scroll_offset = 0;

    char detail_lines[MAX_LINES][MAX_LINE_LEN];
    int total_detail_lines = get_device_details(devices[highlight].name, detail_lines);

    while (1) {
        // Devices box
        werase(dev_win);
        box(dev_win, 0, 0);
        mvwprintw(dev_win, 0, 2, " Devices ");
        for (int i = 0; i < device_count; i++) {
            if (i == highlight) wattron(dev_win, A_REVERSE);
            mvwprintw(dev_win, i+1, 2, "%s", devices[i].name);
            wattroff(dev_win, A_REVERSE);
        }
        wrefresh(dev_win);

        // Details box
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
            // Section headers
            if (strncmp(temp, "===", 3) == 0) {
                wattron(detail_win, A_BOLD | A_UNDERLINE);
                mvwprintw(detail_win, line_no++, 1, "%s", temp);
                wattroff(detail_win, A_BOLD | A_UNDERLINE);
            }
            // Separator lines ("---")
            else if (strcmp(temp, "---") == 0) {
                wattron(detail_win, COLOR_PAIR(1) | A_BOLD);
                mvwprintw(detail_win, line_no++, 1, "%s", temp);
                wattroff(detail_win, COLOR_PAIR(1) | A_BOLD);
            }
            else {
                mvwprintw(detail_win, line_no++, 1, "%s", temp);
            }
        }

        // Page indicator
        mvwprintw(detail_win, height - 1, 2, "[%d/%d]", scroll_offset+1, total_detail_lines);
        wrefresh(detail_win);

        // Input
        ch = getch();
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
            case 'q': case 'Q': endwin(); return 0;
        }
    }
    endwin();
    return 0;
}

