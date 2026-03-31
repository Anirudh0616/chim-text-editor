#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h> // for atexit()
#include <string.h>
#include <sys/_types/_ssize_t.h>
#include <sys/ioctl.h>
#include <sys/ttycom.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define chim_version "0.0.1"
#define chim_tab_stop 4

#define CTRL_KEY(k) ((k) & 0x1f)

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

typedef struct erow {
	int size;
	int rsize;
	char* chars;
	char* render;
} erow;

struct editorConfig {
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	int mode; // 0 = Normal , 1 = insert
	erow* row;
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
	return rx;
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
void editorAppendRow(char* s, size_t len)
{
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
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
}

// EDITOR OPERATIONS
void editorInsertChar(int c)
{
	if (E.cy == E.numrows) {
		editorAppendRow("", 0);
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

// file input o/p
//

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
		editorAppendRow(line, linelen);
	}
	free(line);
	fclose(fp);
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
			if (E.numrows == 0 && y == E.screenrows / 3) {
				char welcome[80];
				int welcomelen
					= snprintf(welcome, sizeof(welcome), "Chim Editor -- version %s", chim_version);
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
	int filelen
		= snprintf(fileName, sizeof(fileName), "%.20s", E.filename ? E.filename : "[No Name]");

	// Show line number
	char lineStats[10];
	int lslen = snprintf(lineStats, sizeof(lineStats), "%d|%d", E.cy + 1, E.numrows);

	abAppend(ab, " ", 1);
	abAppend(ab, " ", 1);
	abAppend(ab, fileName, filelen);
	// Draw bar
	int len = modeShow;
	while (len < E.screencols - filelen - lslen - 3) {
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

void editorMoveCursor(int key)
{

	erow* row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch (key) {
	case ARROW_LEFT:
		if (E.cx != 0) {
			E.cx--;
		}
		break;
	case ARROW_UP:
		if (E.cy != 0) {
			E.cy--;
		}
		break;
	case ARROW_DOWN:
		if (E.cy < E.numrows) {
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
		E.cx = rowlen;
	}
}

void editorProcessKeypress(void)
{

	int c = editorReadKey();

	if (E.mode == 0) {
		switch (c) {
		case '\r':
			// Todo
			break;

		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4); // clear screen
			write(STDOUT_FILENO, "\x1b[H", 3); // put cursor at the top
			exit(0);
			break;
		case CTRL_KEY('I'):
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

		case CTRL_KEY('A'): {
			if (E.cy < E.numrows) {
				E.cx = E.row[E.cy].size;
			}
			E.mode = 1;
		} break;

		case CTRL_KEY('G'):
			if (E.mode == 0) {
				E.cy = 0;
			}
			break;

		case HOME_KEY:
			E.cx = 0;
			break;
		case END_KEY:
			if (E.cy < E.numrows) {
				E.cx = E.row[E.cy].size;
			}
			break;

		case BACKSPACE:
		// case CTRL_KEY('h'):
		case DEL_KEY:
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
		if (c == CTRL_KEY('C') || c == '\x1b') {
			E.mode = 0;
		} else {
			editorInsertChar(c);
		}
	}
}

void initEditor(void)
{
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.numrows = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.row = NULL;
	E.filename = NULL;
	E.mode = 0;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1)
		die("getWindowSize");
	E.screenrows -= 2;
}

int main(int argc, char* argv[])
{

	enableRawMode();
	initEditor();
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("ctrl+q = quit");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0;
}
