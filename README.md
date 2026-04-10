# Chim -- Controlled Vim
A text editor built from scratch in C. Real blood, sweat and tears. 

Ctrl Key shortcuts like Emacs. 

Fast movements and modes like vim.

Feel the speed.

---

## How to Install

#### Using Homebrew (macOS)

```bash
brew tap Anirudh0616/chim

brew install
```
Then you can simply run using 

```bash
chim
```
you can add the file path with a space after chim to open a file.

---

#### Build it yourself 
clone the files somwhere and cd into the repo, then

```bash
make
```

```bash
./chim [filename]
```

or you can just run chim, to see our landing screen.
```bash
./chim
```

---
## Some Shortcuts

### While in Normal Mode

| Shortcut     | Action                                                |
|--------------|-------------------------------------------------------|
| h            | move left                                             |
| j            | move down                                             |
| k            | move up                                               |
| l            | move right                                            |
| ctrl + w     | save                                                  |
| ctrl + q     | Quit                                                  |
| i            | Enter Insert mode                                     |
| a            | Enter Insert mode with append                         |
| ctrl + i     | Go to the START of the line, and go to insert mode    |
| ctrl + a     | Go to the END of the line, and go to insert mode      |
| ctrl + d     | scroll down half a page                               |
| ctrl + u     | scroll up half a page                                 |
| ctrl + g     | go to the bottom of the file                          |
| x            | delete current character                              |
| gg           | go to the bottom of the file                          |
| dd           | delete current line                                   |
| dj           | delete current line and line below                    |
| dk           | delete current line and line above                    |

---

### While in Insert Mode

| Shortcut | Action                                      |
|----------|---------------------------------------------|
| ctrl + c | Exit insert mode, Enter Normal Mode         |
| Esc      | exit insert mode                            |


---

### Future Additions 

- [x] Vim Shortcuts
- [x] Hello Line numbers!
- [ ] Editor Search
- [x] Homebrew tap
- [ ] Syntax Highlighting 
- [ ] Visual Mode
- [ ] Scripting Language

---

Made while bored in class.
