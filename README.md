# Remote File Sync  

**Technical concepts**: TCP Sockets, POSIX Threads, Concurrency, Fork (executive commands), Inotify, File/path manipulation

Mutli-client file sharing system written in C\
Clients spawn a local file, `~/rfs/main.py`, and watches for modifications\
Clients connect to a central server and sends these modifications to the server in real time\
Clients fetches new changes from server and applies them to local file\
Server receives file data and merges it with server-local file\
Server broadcasts changes to all connected clients\
Client creates three pthreads (filewatch, send, and receive), and the server uses one thread per client

**MVP**: Two people can edit a file through their clients while connected to the central server.

**Demo**: [insert link here]

## Features
- Multi-client TCP Sockets 
- Inotify to watch modifications of file
- Ensuring ~/rfs/main.py file exists (working on all linux devices)
- Client has send/receive threads
- Server creates pthread for every client
- Forking to use executive commands of creating files/folders
- Using executive commands to accomplish git merge
- Safe shutdown when using `Ctrl+C` to interrupt

## Build + Quickstart


#### Git clone the repository (steps for both client and server)

```bash
# 1. Clone the repository
git clone https://github.com/titut/remote_file_sync.git
cd remote_file_sync
```

#### Make a build directory and build it (steps for both client and server)

1. Make build directory (skip if build folder already exists)
```bash
mkdir build
cd build
```

2. Run Cmake in the build directory on the outer directory
```bash
cmake ..
```

3. Build the directory
```bash
make
```

#### Now, run the project:
4. Run the server on **raspberry pi** (from the build directory)
```bash
./bin/server
```

5. Run the client on **linux device** (on two different devices from the build directory)
```bash
./bin/client
```

6. Edit rfs.py (you can open it up in *IDE or use vim, etc.)
```bash
sudo nano ~/rfs/main.py
```

7. See the changes on the other devices `~/rfs/main.py`

* If you chosoe to open it in an IDE, ensure to set the Autosave delay to very fast (~100ms) for the most "Google Doc" like experience.

## Contributors
**Bill Le**: https://bill-le.info

**Darian Jimenez**: https://github.com/darianjimenez

## Sources

1. How to use inotify: https://www.thegeekstuff.com/2010/04/inotify-c-program-example/
2. Poll: https://pubs.opengroup.org/onlinepubs/009696799/functions/poll.html
3. Pipe: https://www.geeksforgeeks.org/c/pipe-system-call/
4. Safe ctrl + C: https://stackoverflow.com/questions/7623401/override-ctrl-c
5. Check if file exists: https://www.learnc.net/c-tutorial/c-file-exists/
6. Atomic write file: https://stackoverflow.com/questions/57383333/how-to-write-a-file-atomically-and-asynchronously-in-linux-using-c
7. Socket quick-start: https://www.geeksforgeeks.org/c/socket-programming-cc/
8. Socket reading from class: Head First C Chapter 11
9. Threads reading: Head First C Chapter 12
10. Lib: https://manual.cs50.io/
11. Networking: https://beej.us/guide/bgnet/ 
12. ChatGPT help with fixing errors: https://chatgpt.com/share/693c4672-6120-8012-ba3b-4c7947482686 