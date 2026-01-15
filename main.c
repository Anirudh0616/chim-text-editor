#include <termios.h>
#include <unistd.h>
#include <stdlib.h> // for atexit()
#include <stdio.h>
#include <ctype.h>

struct termios orig_termios;

void disableRawMode(void){
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode(void){

    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(IXON);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG); // remove Echo and canonical Mode

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main(void){

    enableRawMode();

    char c;
    while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q'){
        if(iscntrl(c)) {
            printf("%d\n", c);
        } else {
            printf("%d ('%c')\n", c , c);
        }
    }
    return 0;
}
