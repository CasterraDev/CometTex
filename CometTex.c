#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

#define COMETTEX_VERSION "0.0.1"
#define COMETTEX_QUIT_TIMES 3;
#define COMETTEX_TAB_STOP 8
#define CTRL_KEY(c) ((c) & 0x1f)

typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

struct editorConfig{
    int mx,my;
    int rx;
    int colOffset;
    int rowOffset;
    int screenRow;
    int screenCol;
    int numRows;
    erow *row;
    int dirty;
    char *filename;
    char statusMsg[80];
    time_t statusMsg_time;
    struct termios orignal_termios;
};

enum editorKey{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

struct editorConfig E;

//-----------------------Prototypes-------------------------
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

struct abuf{
    char *b;
    int len;
};

#define ABUF_INIT {NULL,0}

void abAppend(struct abuf *ab,const char *s,int len){
    char *new = realloc(ab->b,ab->len + len);

    if(new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab){
    free(ab->b);
}

void clearScreen(){
    //Clear the entire screen
    write(STDOUT_FILENO, "\x1b[2H", 4);
    //Reposition the cursor to the top left
    write(STDOUT_FILENO, "\x1b[H",3);
}

void die(const char *s){
    clearScreen();
    perror(s);
    exit(1);
}

void disableRawMode(){
    if( tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orignal_termios) == -1) die("DisableRawMode() Failed");
}

void enableRawMode(){
    if (tcgetattr(STDIN_FILENO, &E.orignal_termios) == -1) die("EnableRawMode() tcgetattr Failed");
    //atexit is called when the program exits
    atexit(disableRawMode);

    struct termios raw = E.orignal_termios;
    //Turn off some flags
    raw.c_cflag |= (CS8);
    raw.c_oflag &= ~(OPOST); //OPOST turns off output processing
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP); //IXON disables ctrl-s and ctrl-q,ICRNL fixes ctrl-m
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); //Turn off Echo,ICANON,ISIG disables ctrl-c, ctrl-z,IEXTEN disables ctrl-v

    raw.c_cc[VMIN] = 0; //Min amount of bytes read() needs before it can return
    raw.c_cc[VTIME] = 1; //Max amount of time read() waits before it returns

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("EnableRawMode() tcsetattr Failed");
}

int editorRowMxToRx(erow *row, int mx){
    int rx = 0;
    for (int i = 0;i<mx;i++){
        if (row->chars[i] == '\t'){
            rx += (COMETTEX_TAB_STOP - 1) - (rx % COMETTEX_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int editorRowRxtoMx(erow *row, int rx){
    int cur_rx = 0;
    int mx;
    for (mx = 0;mx < row->size;mx++){
        if (row->chars[mx] == '\t'){
            cur_rx += (COMETTEX_TAB_STOP - 1) - (cur_rx % COMETTEX_TAB_STOP);
        }
        cur_rx++;

        if (cur_rx > rx) return mx;
    }
    return mx;
}

void editorScroll(){
    E.rx = 0;
    if (E.my < E.numRows){
        E.rx = editorRowMxToRx(&E.row[E.my], E.mx);
    }

    //Vertical Scrolling
    if (E.my < E.rowOffset){
        E.rowOffset = E.my;
    }
    if (E.my >= E.rowOffset + E.screenRow){
        E.rowOffset = E.my - E.screenRow + 1;
    }
    //Horizontal Scrolling
    if (E.rx < E.colOffset){
        E.colOffset = E.mx;
    }
    if (E.rx >= E.colOffset + E.screenCol){
        E.colOffset = E.rx - E.screenCol + 1;
    }
}

void editorDrawRow(struct abuf *ab){
    for(int i = 0;i<E.screenRow;i++){
        int fileRow = i + E.rowOffset;
        if (fileRow >= E.numRows){
            if (E.numRows == 0 && i == E.screenRow / 3){
                char welcome[124];
                int welcomeLen = snprintf(welcome, sizeof(welcome), "CometTex Editor -- Version %s", COMETTEX_VERSION);
                if (welcomeLen > E.screenCol){
                    welcomeLen = E.screenCol;
                }
                int padding = (E.screenCol - welcomeLen)/2;
                if (padding){
                    abAppend(ab,"~",1);
                    padding--;
                }
                while(padding--) abAppend(ab, " ", 1);
                abAppend(ab,welcome, welcomeLen);
            }else{
                abAppend(ab, "~", 1);
            }
        }else{
            int len = E.row[fileRow].rsize - E.colOffset;
            if (len < 0) len = 0;
            if (len > E.screenCol) len = E.screenCol;
            abAppend(ab, &E.row[fileRow].render[E.colOffset], len);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];

    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numRows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d%d", E.my + 1, E.numRows);

    if (len > E.screenCol) len = E.screenCol;
    abAppend(ab, status, len);
    while (len < E.screenCol){
        if (E.screenCol - len == rlen){
            abAppend(ab, rstatus, rlen);
            break;
        }else{
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab){
    abAppend(ab, "\x1b[K", 3);
    int msgLen = strlen(E.statusMsg);
    if (msgLen > E.screenCol) msgLen = E.screenCol;
    if (msgLen && time(NULL) - E.statusMsg_time < 5){
        abAppend(ab, E.statusMsg, msgLen);
    }
}

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRow(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.my - E.rowOffset) + 1, (E.rx - E.colOffset) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusMsg, sizeof(E.statusMsg), fmt, ap);
    va_end(ap);
    E.statusMsg_time = time(NULL);
}

int editorReadKey(){
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if (nread == 1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b'){
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '['){
            if (seq[1] >= '0' && seq[1] <= '9'){
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~'){
                    switch(seq[1]){
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }else{
                switch (seq[1]){
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O'){
            switch(seq[1]){
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }else{
        return c;
    }
}

char *editorPrompt(char *prompt, void (*callback)(char *, int)){
    size_t bufSize = 128;
    char *buf = malloc(bufSize);

    size_t bufLen = 0;
    buf[0] = '\0';

    while (1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if (bufLen != 0) buf[--bufLen] = '\0';
        }else if (c == '\x1b'){
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        }else if (c == '\r'){
            if (bufLen != 0){
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        }else if (!iscntrl(c) && c < 128){
            if (bufLen == bufSize - 1){
                bufSize *= 2;
                buf = realloc(buf, bufSize);
            }
            buf[bufLen++] = c;
            buf[bufLen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

void editorMoveCursor(int key){
    erow *row = (E.my >= E.numRows) ? NULL : &E.row[E.my];

    switch(key){
        case ARROW_LEFT:
            if (E.mx != 0){
                E.mx--;
            }else if (E.my > 0){
                E.my--;
                E.mx = E.row[E.my].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.mx < row->size){
                E.mx++;
            }else if (row && E.mx == row->size){
                E.my++;
                E.mx = 0;
            }
            break;
        case ARROW_UP:
            if (E.my != 0){
                E.my--;
            }
            break;
        case ARROW_DOWN:
            if (E.my < E.numRows){
                E.my++;
            }
            break;
    }

    row = (E.my >= E.numRows) ? NULL : &E.row[E.my];
    int rowlen = row ? row->size : 0;
    if (E.mx > rowlen){
        E.mx = rowlen;
    }
}

int getCursorPos(int *rows,int *cols){
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n",4) != 4) return -1;

    while (i < sizeof(buf) - 1){
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows,int *cols){
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B",12) != 12) return -1;
        return getCursorPos(rows,cols);
    }else{
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void editorUpdateRow(erow *row){
    int tabs = 0;
    for (int i = 0;i<row->size;i++){
        if (row->chars[i] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(COMETTEX_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int i = 0;i<row->size;i++){
        if (row->chars[i] == '\t'){
            row->render[idx++] = ' ';
            while (idx % COMETTEX_TAB_STOP != 0) row->render[idx++] = ' ';
        }else{
            row->render[idx++] = row->chars[i];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len){
    if (at < 0 || at > E.numRows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numRows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numRows++;
    E.dirty++;
}

void editorFreeRow(erow *row){
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at){
    if (at < 0 || at >= E.numRows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numRows - at - 1));
    E.numRows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c){
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len){
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at){
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

void editorInsertChar(int c){
    if (E.my == E.numRows){
        editorInsertRow(E.numRows, "", 0);
    }
    editorRowInsertChar(&E.row[E.my], E.mx, c);
    E.mx++;
}

void editorInsertNewLine(){
    if (E.mx == 0){
        editorInsertRow(E.my, "", 0);
    }else{
        erow *row = &E.row[E.my];
        editorInsertRow(E.my + 1, &row->chars[E.mx], row->size - E.mx);
        row = &E.row[E.my];
        row->size = E.mx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.my++;
    E.mx = 0;
}

void editorDelChar(){
    if (E.my == E.numRows) return;
    if (E.mx == 0 && E.my == 0) return;

    erow *row = &E.row[E.my];
    if (E.mx > 0){
        editorRowDelChar(row, E.mx - 1);
        E.mx--;
    }else{
        E.mx = E.row[E.my - 1].size;
        editorRowAppendString(&E.row[E.my - 1], row->chars, row->size);
        editorDelRow(E.my);
        E.my--;
    }
}

char *editorRowsToString(int *buflen){
    int totlen = 0;
    for (int i = 0;i<E.numRows;i++){
        totlen += E.row[i].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (int i = 0;i<E.numRows;i++){
        memcpy(p, E.row[i].chars, E.row[i].size);
        p += E.row[i].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void editorOpen(char *filename){
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1){
        while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')){
            linelen--;
        }
        editorInsertRow(E.numRows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave(){
    if (E.filename == NULL){
        E.filename = editorPrompt("Save as: %s (ESC to cancel", NULL);
        if (E.filename == NULL){
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1){
        if (ftruncate(fd, len) != -1){
            if (write(fd, buf, len) == len){
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
       close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't Save! I/O error %s", strerror(errno));
}

void editorFindCallback(char *query, int key){
    static int last_match = -1;
    static int direction = 1;

    if (key == '\r' || key == '\x1b'){
        last_match = -1;
        direction = 1;
        return;
    }else if (key == ARROW_RIGHT || key == ARROW_DOWN){
        direction = 1;
    }else if (key == ARROW_LEFT || key == ARROW_UP){
        direction = -1;
    }else{
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int cur = last_match;

    for (int i = 0;i<E.numRows;i++){
        cur += direction;
        if (cur == -1) cur = E.numRows - 1;
        else if (cur == E.numRows) cur = 0;

        erow *row = &E.row[cur];
        char *match = strstr(row->render, query);
        if (match){
            last_match = cur;
            E.my = cur;
            E.mx = editorRowRxtoMx(row, match - row->render);
            E.rowOffset = E.numRows;
            break;
        }
    }
}

void editorFind(){
    int saved_mx = E.mx;
    int saved_my = E.my;
    int saved_colOff = E.colOffset;
    int saved_rowOff = E.rowOffset;

    char *query = editorPrompt("Search: %s (ESC to cancel", editorFindCallback);

    if (query){
        free(query);
    }else{
        E.mx = saved_mx;
        E.my = saved_my;
        E.colOffset = saved_colOff;
        E.rowOffset = saved_rowOff;
    }
}

void editorProcessKeypress(){
    static int quit_times = COMETTEX_QUIT_TIMES;

    int c = editorReadKey();


    switch(c){
        case '\r':
            editorInsertNewLine();
            break;
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0){
                editorSetStatusMessage("WARNING!! File modified. Press Ctrl+Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H",3);
            exit(0);
            break;
        
        case CTRL_KEY('s'):
            editorSave();
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case CTRL_KEY('x'):
            editorSave();
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H",3);
            exit(0);
            break;
        
        case HOME_KEY:
            E.mx = 0;
            break;
        case END_KEY:
            if (E.my < E.numRows){
                E.mx = E.row[E.my].size;
            }
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP){
                    E.my = E.rowOffset;
                }else if (c == PAGE_DOWN){
                    E.my = E.rowOffset + E.screenRow - 1;
                    if (E.my > E.numRows) E.my = E.numRows;
                }
                int t = E.screenRow;
                while(t--){
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        
        case CTRL_KEY('l'):
        case '\x1b':

            break;
        default:
            editorInsertChar(c);
            break;
    }

    quit_times = COMETTEX_QUIT_TIMES;
}

void initEditor(){
    E.mx = 0;
    E.my = 0;
    E.rx = 0;
    E.rowOffset = 0;
    E.colOffset = 0;
    E.numRows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusMsg[0] = '\0';
    E.statusMsg_time = 0;

    if (getWindowSize(&E.screenRow, &E.screenCol) == -1) die("getWindowSize");
    E.screenRow -= 2;
}

int main(int argc, char *argv[]){
    enableRawMode();
    initEditor();
    if (argc >= 2){
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl+S = save | CTRL+F find | Ctrl+Q = quit");

    while (1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}