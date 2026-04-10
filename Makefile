main: main.c
	$(CC) main.c options.c -o chim -Wall -Wextra -pedantic -std=c99
