#include <sys/fcntl.h>
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include "options.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h> // for atexit()
#include <string.h>
#include <sys/_types/_ssize_t.h>
#include <sys/ioctl.h>
#include <sys/ttycom.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define chim_version "0.1.2"
#define chim_tab_stop 4 // tab stop width
#define chim_quit_times 2

#define CTRL_KEY(k) ((k) & 0x1f)

int digitCounter(int n)
{
	if (n == 0)
		return 1;
	int c = 0;
	while (n) {
		c++;
		n /= 10;
	}
	return c;
}

enum editorKey {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN,
};

struct termios orig_termios;

void die(const char* s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4); // clear screen
	write(STDOUT_FILENO, "\x1b[H", 3); // put cursor at the top

	perror(s);
	exit(1);
}

// Editor row - stores line of text as pointer
//
void editorSetStatusMessage(const char* fmt, ...);
char* editorPrompt(char* prompt, bool fromSave, void (*callback)(char *, int));
void editorMoveCursor(int key);
void editorInsertNewLine(void);

typedef struct erow {
	int size;
	int rsize;
	char* chars;
	char* render;
} erow;

struct editorConfig {
	int numberGutter;
	int zenMode;
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	int mode; // 0 = Normal , 1 = insert
	erow* row;
	int dirty;
	char* filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

struct editorConfig E;

void disableRawMode(void)
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
		die("tcsetattr");
	}
}

void enableRawMode(void)
{

	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
		die("tcgetattr");
	}
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // disables ctrl S and ctrl Q
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // remove Echo and canonical Mode

	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
		die("tcsetattr");
	}
}

int editorReadKey(void)
{
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1) != 1)) {
		if ((nread == -1 && errno != EAGAIN))
			die("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1)
			return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1)
			return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
					case '1':
						return HOME_KEY;
					case '3':
						return DEL_KEY;
					case '4':
						return END_KEY;
					case '5':
						return PAGE_UP;
					case '6':
						return PAGE_DOWN;
					case '7':
						return HOME_KEY;
					case '8':
						return END_KEY;
					}
				}
			} else {
				switch (seq[1]) {
				case 'A':
					return ARROW_UP;
				case 'B':
					return ARROW_DOWN;
				case 'C':
					return ARROW_RIGHT;
				case 'D':
					return ARROW_LEFT;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
			case 'H':
				return HOME_KEY;
			case 'F':
				return END_KEY;
			}
		}

		return '\x1b';
	} else {
		switch (c) {
		default:
			return c;
		}
	}
}

int getCursorPosition(int* rows, int* cols)
{
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n]", 4) != 4)
		return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[')
		return -1;
	if (sscanf(&buf[2], "%d;%d", cols, rows) != 2)
		return -1;

	return 0;
}

int getWindowSize(int* rows, int* cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B]]", 12) != 12)
			return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

// row operations
//
int editorRowCxToRx(erow* row, int cx)
{
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t') {
			rx += (chim_tab_stop - 1) - (rx % chim_tab_stop);
		}
		rx++;
	}
	return rx + E.numberGutter;
}

int editorRowRxToCx(erow * row, int rx){
    int cur_rx = 0;
    int cx;
    for(cx = 0;cx<row->size;cx++){
        if(row->chars[cx] == '\t')
            cur_rx += (chim_tab_stop - 1) - (cur_rx % chim_tab_stop);
        cur_rx++;

        if(cur_rx > rx) return cx;
    }
    return cx;
}

void editorUpdateRow(erow* row)
{

	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t')
			tabs++;
	}

	free(row->render);
	row->render = malloc(row->size + tabs * (chim_tab_stop - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % chim_tab_stop != 0)
				row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

// new erow constructor
void editorInsertRow(int at, char* s, size_t len)
{
	if (at < 0 || at > E.numrows)
		return;
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
	E.dirty++;
}

void editorFreeRow(erow* row)
{
	free(row->render);
	free(row->chars);
}

void editorddRow(int at)
{
	if (at < 0 || at >= E.numrows)
		return;
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	if (E.numrows <= 0) {
		editorInsertRow(0, "", 0);
		editorMoveCursor(ARROW_DOWN);
		editorMoveCursor(ARROW_UP);
	} else if (at >= E.numrows) {
		editorMoveCursor(ARROW_UP);
	}
	E.dirty++;
}

void editorDelRow(int at)
{
	if (at < 0 || at >= E.numrows)
		return;
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	// if (E.numrows <= 0) {
	// 	editorInsertRow(0, "", 0);
	// 	editorMoveCursor(ARROW_DOWN);
	// 	editorMoveCursor(ARROW_UP);
	// } else if (at >= E.numrows) {
	// 	editorMoveCursor(ARROW_UP);
	// }
	E.dirty++;
}

void editorRowInsertChar(erow* row, int at, int c)
{
	if (at < 0 || at > row->size)
		at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowAppendString(erow* row, char* s, size_t len)
{
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowDelChar(erow* row, int at)
{
	if (at < 0 || at > row->size)
		at = row->size;
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	editorUpdateRow(row);
	E.dirty++;
}

// EDITOR OPERATIONS
void editorInsertChar(int c)
{
	if (E.cy == E.numrows) {
		editorInsertRow(E.numrows, "", 0);
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

void editorInsertNewLine(void)
{
	if (E.cx == 0) {
		editorInsertRow(E.cy, "", 0);
	} else {
		erow* row = &E.row[E.cy];
		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
		row = &E.row[E.cy]; // make sure the pointer is in the right spot
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editorUpdateRow(row);
	}
	E.cy++;
	E.cx = 0;
}

void editorDelChar(void)
{
	if (E.cx == 0 && E.cy == 0)
		return;
	if (E.cy == E.numrows)
		return;
	erow* row = &E.row[E.cy];
	if (E.cx > 0) {
		editorRowDelChar(row, E.cx - 1);
		E.cx--;
	} else {
		E.cx = E.row[E.cy - 1].size;
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDelRow(E.cy);
		E.cy--;
	}
}

// file input o/p
//
char* editorRowsToString(int* buflen)
{
	int totlen = 0;
	int j;
	for (j = 0; j < E.numrows; j++) {
		totlen += E.row[j].size + 1;
	}
	*buflen = totlen;

	char* buf = malloc(*buflen);
	char* p = buf;
	for (j = 0; j < E.numrows; j++) {
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void editorOpen(char* filename)
{
	free(E.filename);
	E.filename = strdup(filename);
	FILE* fp = fopen(filename, "r");
	if (!fp)
		die("fopen");

	char* line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while ((linelen > 0 && (line[linelen - 1] == '\n')) || line[linelen - 1] == '\r')
			linelen--;
		editorInsertRow(E.numrows, line, linelen);
	}
	int s = digitCounter(E.numrows);
	E.numberGutter = (s > 3 ? s : 3) + 1;
	free(line);
	fclose(fp);
	E.dirty = 0;
}

void editorSave(void)
{
	if (E.filename == NULL) {

		E.filename = editorPrompt("Save as: %s", true, NULL);
		if (E.filename == NULL) {
			editorSetStatusMessage("Save aborted");
			return;
		}
	}

	int len;
	char* buf = editorRowsToString(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				E.dirty = 0;
				editorSetStatusMessage("\"%s\" %dL, %dB written", E.filename, E.numrows, len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void editorFindCallback(char* query, int key)
{
    static int last_match = -1;
    static int direction = 1;

	if (key == '\r' || key == '\x1b'){
        last_match = -1;
        direction = 1;
		return;
    } else if(key == CTRL_KEY('n')){
        direction = 1;
    } else if(key == CTRL_KEY('p')){
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }
    
    if(last_match == -1)direction = 1;
    int current = last_match;
	int i;
	for (i = 0; i < E.numrows; i++) {
        current += direction;
        if(current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;
		erow* row = &E.row[current];
		char* match = strstr(row->render, query);
		if (match) {
            last_match = current;
            E.cy = current;
			E.cx = editorRowRxToCx(row, match - row->render);
			E.rowoff = E.numrows;
			break;
		}
	}
}
void editorFind(void){
    
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;


	char* query = editorPrompt("Search: %s", true, editorFindCallback);
    if(query){
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;

    }
}

// append buffer ( Dynamic String)
struct abuf {
	char* b;
	int len;
};

#define ABUF_INIT { NULL, 0 }

void abAppend(struct abuf* ab, const char* s, int len)
{
	char* newch = (char*)realloc(ab->b, ab->len + len);

	if (newch == NULL)
		return;
	memcpy(&newch[ab->len], s, len);
	ab->b = newch;
	ab->len += len;
}

void abFree(struct abuf* ab) { free(ab->b); }

void editorScroll(void)
{
	E.rx = 0;
	if (E.cy < E.numrows) {
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

	if (E.cy < E.rowoff) {
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		E.rowoff = E.cy - E.screenrows + 1;
	}
	if (E.rx < E.coloff) {
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols) {
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuf* ab)
{
	int y;
	for (y = 0; y < E.screenrows; y++) {
		int filerow = y + E.rowoff;
		if (filerow >= E.numrows) {
			if (E.numrows <= 1 && y == E.screenrows / 3) {
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome), "Chim Editor -- version %s", chim_version);
				if (welcomelen > E.screencols)
					welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen) / 2;
				if (padding) {
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--)
					abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			} else {
				abAppend(ab, "~", 1);
			}
		} else {

			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0)
				len = 0;
			if (len > E.screencols)
				len = E.screencols;
			if (!E.zenMode) {
				int s = digitCounter(filerow + 1);
				for (int i = 0; i < E.numberGutter - s - 1; i++) {
					abAppend(ab, " ", 1);
				}
				char lineNumber[10];
				int lineNumberSize = snprintf(lineNumber, sizeof(lineNumber), "%d", filerow + 1);
				abAppend(ab, lineNumber, lineNumberSize);
				abAppend(ab, " ", 1);
			}
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}

		abAppend(ab, "\x1b[K", 3); // Clear each line individually while redrwaing
								   // instead of clear screen
		if (y < E.screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}

void editorDrawStatusBar(struct abuf* ab)
{
	// Show the current mode
	char modeS[10];
	int modeShow;
	abAppend(ab, "\x1b[1m", 4);
	abAppend(ab, "\x1b[7m", 4);
	if (E.mode == 0) {
		modeShow = snprintf(modeS, sizeof(modeS), "NORMAL");
	} else {
		modeShow = snprintf(modeS, sizeof(modeS), "INSERT");
	}
	abAppend(ab, " ", 1);
	abAppend(ab, modeS, modeShow);
	// Show the file name
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\x1b[7m", 4);
	char fileName[20];
	int filelen = snprintf(fileName, sizeof(fileName), "%.20s", E.filename ? E.filename : "[No Name]");

	// modified (dirty) indicator
	char dirty[4];
	int dirtylen = snprintf(dirty, sizeof(dirty), "%s", E.dirty ? "[+]" : "");

	// Show line number
	char lineStats[10];
	int lslen = snprintf(lineStats, sizeof(lineStats), "%d|%d", E.cy + 1, E.numrows);

	abAppend(ab, " ", 1);
	abAppend(ab, " ", 1);
	abAppend(ab, fileName, filelen);
	abAppend(ab, dirty, dirtylen);
	abAppend(ab, " ", 1);
	// Draw bar
	int len = modeShow;
	while (len < E.screencols - filelen - lslen - 4 - dirtylen) {
		abAppend(ab, " ", 1);
		len++;
	}
	abAppend(ab, lineStats, lslen);
	//
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf* ab)
{
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols)
		msglen = E.screencols;
	if (msglen && time(NULL) - E.statusmsg_time < 5) {
		abAppend(ab, E.statusmsg, msglen);
	}
}

void editorRefreshScreen(void)
{
	if (E.zenMode) {
		E.numberGutter = 0;
	} else {
		int s = digitCounter(E.numrows);
		E.numberGutter = (3 > s ? 3 : s) + 1;
	}
	editorScroll();

	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6); // hide cursor
	abAppend(&ab, "\x1b[H", 3); // put cursor at the top

	editorDrawRows(&ab);
	abAppend(&ab, "\r\n", 2);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1,
		E.rx - E.coloff + 1); // Terminal uses 1 indexstart value. How unfortunate
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6); // un-hide cursor

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorSetStatusMessage(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

// INPUt

char* editorPrompt(char* prompt, bool fromSave, void(*callback)(char*, int))
{
	size_t bufsize = 128;
	char* buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while (1) {
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();
		if (fromSave) {
			if (c == BACKSPACE || c == DEL_KEY) {
				if (buflen != 0)
					buf[--buflen] = '\0';
			} else if (c == '\x1b' || c == CTRL_KEY('c')) {
				editorSetStatusMessage("");
                if(callback) callback(buf,c);
				free(buf);
				return NULL;
			} else if (c == '\r') {
				if (buflen != 0) {
					editorSetStatusMessage("");
                    if(callback) callback(buf,c);
					return buf;
				}
			} else if (!iscntrl(c) && c < 128) {
				if (buflen == bufsize - 1) {
					bufsize *= 2;
					buf = realloc(buf, bufsize);
				}
				buf[buflen++] = c;
				buf[buflen] = '\0';
			}
		} else {
			if (strcmp(prompt, "d") == 0) {
				if (c == 'd') {
					editorddRow(E.cy);
				} else if (c == 'j') {
					int at = E.cy;
					editorddRow(E.cy);
					editorddRow(at);
				} else if (c == 'k') {
					editorMoveCursor(ARROW_UP);
					int at = E.cy;
					editorddRow(E.cy);
					editorddRow(at);
				}
			} else if (strcmp(prompt, "g") == 0) {
				if (c == 'g') {
					E.cy = 0;
				}
			// } else if (strcmp(prompt, "/") == 0) {
			// 	if (c == 'g') {
			// 		E.cy = 0;
			// 	}
			} else if (c == '\x1b' || c == CTRL_KEY('c')) {
				free(buf);
				editorSetStatusMessage("");
                if(callback) callback(buf,c);
				return NULL;
			}
			editorSetStatusMessage("");
            if(callback) callback(buf,c);
			free(buf);
			return NULL;
		}

        if(callback) callback(buf,c);
	}
}

void editorMoveCursor(int key)
{

	erow* row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch (key) {
	case ARROW_LEFT:
		if (E.cx) {
			E.cx--;
		}
		break;
	case ARROW_UP:
		if (E.cy != 0) {
			E.cy--;
		}
		break;
	case ARROW_DOWN:
		if (E.cy < E.numrows - 1) {
			E.cy++;
		}
		break;
	case ARROW_RIGHT:
		if (row && E.cx < row->size - 1) {
			E.cx++;
		}
		break;
	}

	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx >= rowlen) {
		E.cx = rowlen > 0 ? rowlen - 1 : 0;
	}
}

void editorProcessKeypress(void)
{
	static int quit_times = chim_quit_times;

	int c = editorReadKey();

	if (E.mode == 0) {
		switch (c) {
		case '\r': // enter
			editorMoveCursor(ARROW_DOWN);
			break;
		case 'd':
			editorPrompt("d", false, NULL);
			break;
		case 'g':
			editorPrompt("g", false, NULL);
			break;
        case '/':
            // editorPrompt("/", false);
            editorFind();
            break;

		case CTRL_KEY('q'):
			if (E.dirty && quit_times) {
				editorSetStatusMessage("File has unsaved changes, quit %d more times to force quit", quit_times);
				quit_times--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4); // clear screen
			write(STDOUT_FILENO, "\x1b[H", 3); // put cursor at the top
			exit(0);
			break;
		case CTRL_KEY('w'):
			editorSave();
			break;
		case 'i':
			E.mode = 1;
			break;
		case 'a':
			if (E.cy < E.numrows && E.cx < E.row[E.cy].size) {
				E.cx++;
			}
			E.mode = 1;
			break;
		case CTRL_KEY('D'): {
			int times = E.screenrows / 2;
			while (times--) {
				editorMoveCursor(ARROW_DOWN);
			}
		} break;
		case CTRL_KEY('U'): {
			int times = E.screenrows / 2;
			while (times--) {
				editorMoveCursor(ARROW_UP);
			}
		} break;

		case CTRL_KEY('I'): {
			E.cx = 0;
			E.mode = 1;
		} break;

		case CTRL_KEY('A'): {
			if (E.cy < E.numrows) {
				E.cx = E.row[E.cy].size;
			}
			E.mode = 1;
		} break;

		case CTRL_KEY('G'):
			E.cy = E.numrows - 1;
			break;

		case HOME_KEY:
			E.cx = 0;
			break;
		case END_KEY:
			if (E.cy < E.numrows) {
				E.cx = E.row[E.cy].size;
			}
			break;

		case 'x':
		case DEL_KEY:
			editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;

		case PAGE_UP:
		case PAGE_DOWN: {
			if (c == PAGE_UP) {
				E.cy = E.rowoff;
			} else if (c == PAGE_DOWN) {
				E.cy = E.rowoff + E.screenrows - 1;
				if (E.cy > E.numrows)
					E.cy = E.numrows;
			}

			int times = E.screenrows;
			while (times--) {
				editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
		} break;

		case 'h':
		case BACKSPACE:
			editorMoveCursor(ARROW_LEFT);
			break;
		case 'j':
			editorMoveCursor(ARROW_DOWN);
			break;
		case 'k':
			editorMoveCursor(ARROW_UP);
			break;
		case 'l':
			editorMoveCursor(ARROW_RIGHT);
			break;
		case 'o': {
			editorInsertRow(E.cy + 1, "", 0);
			E.cy++;
			E.cx = 0;
			E.mode = 1;
		} break;
		case 'O': {
			editorInsertRow(E.cy, "", 0);
			E.cx = 0;
			E.mode = 1;
		} break;
		case CTRL_KEY('z'): {
			E.zenMode = !E.zenMode;
			if (E.zenMode) {
				editorSetStatusMessage("Chim: Inner Peace (Zen Mode On)");
			} else {
				editorSetStatusMessage("Chim: Back to Reality (Zen Mode Off)");
			}
		} break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
			// default:
			//     {
			//         if(E.mode==1){
			//             editorInsertChar(c);
			//         }
			//     }
			//     break;
		}

	} else if (E.mode == 1) {
		switch (c) {
		case CTRL_KEY('c'):
		case '\x1b':
			if (E.cx > 0) {
				E.cx--;
			}
			E.mode = 0;
			break;
		case '\r':
			editorInsertNewLine();
			break;

		case BACKSPACE:
			editorDelChar();
			break;

		default:
			editorInsertChar(c);
			break;
		}
	}
	quit_times = chim_quit_times;
}

void initEditor(void)
{
	E.cy = 0;
	E.cx = 0;
	E.rx = 0;
	E.dirty = 0;
	E.numrows = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.row = NULL;
	E.filename = NULL;
	E.mode = 0;
	E.zenMode = 0;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1)
		die("getWindowSize");
	// editorInsertRow(0, "", 0);
	E.screenrows -= 2;
	editorRefreshScreen();
}

int main(int argc, char* argv[])
{

	if (handleOptions(argc, argv)) {
		return 0;
	}
	enableRawMode();
	initEditor();
	if (argc >= 2) {
		editorOpen(argv[1]);
	} else {
		editorInsertRow(0, "", 0);
		E.dirty = 0;
	}

	editorSetStatusMessage("ctrl+q = quit");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0;
}
