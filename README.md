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
```sdl
write if (receiver == 514 && msg == '7891') then value = 'Not Found' ttl = 300ms
```

## What's MySDL
This database is **My** SDL so I can use it 
for something , I'll use it to cache IPC message<br>
You can use it by [SDLSH](https://github.com/Saladin5101/sdlsh.git) or use [libsdl](https://github.com/Saladin5101/libsdl) library via C.<br>

### How to Use It In a Server
You can `make install` this SDL database into your computer.<br>
Of course, this database is very small, because it is using for my
IPC cache. <br>

### Contribute
#### Report Bug
You can open a issue in issues, but please tell me what happend.<br>
#### Upload Patch
I love mailing list, but this is my toll, so you can use PR to upload
your patch.<br>
### License
We Licensed on GPLv2 or later.