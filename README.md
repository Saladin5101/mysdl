# MySDL
System Database Language's first project

## What's SDL
Ugh, SDL (System Database Language) is a new 
database language for IPC cache for E-comOS.<br>
I don't think the all-capitalization syntax 
of SQL is the right choice (unless you keep 
locking it in capitalization), and some gram-
mars are not in line with the original meaning 
of English words (such as 'WHERE').<br>
SDL's syntax is C-like and Python-like. For 
example: 
## SDL syntax

### write
Store a value, with an optional TTL.
```sdl
write key = 'value'
write key = 'value' ttl = 300ms
write if (key2 == 'x') then key = 'value' ttl = 5s
```

### read
Fetch a value by key, or scan all keys.
```sdl
read key
read *
```

### erase
Delete a key.
```sdl
erase key
```

### flush
Drop every entry in the database.
```sdl
flush
```

### TTL units
`300ms` · `5s` · `1m`

## Build & Install

```sh
make
sudo make install   # installs to /usr/local/bin/mysdl
```

## Usage

Pass an SDL statement as a single argument:
```sh
mysdl "write cache/514:7891 = 'Not Found' ttl = 300ms"
mysdl "read cache/514:7891"
mysdl "erase cache/514:7891"
mysdl "read *"
mysdl "flush"
```

Or use the interactive shell [SDLSH](https://github.com/Saladin5101/sdlsh.git),
or embed via [libsdl](https://github.com/Saladin5101/libsdl).

## Contribute

**Report a bug** — open an issue and describe what happened.

**Send a patch** — open a PR. I love Mailing list but this is my play project , so 
I think you can use Pull Request to contribute your code.

## License

GPL-2.0-or-later