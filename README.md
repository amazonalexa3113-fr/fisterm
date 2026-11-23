# fisterm

a terminal this deadass minimalistic??

## about

fisterm is a terminal emulator project focused on providing a simple, functional terminal experience on linux

## features

- terminal window
- pseudo-terminal (PTY) support
- shell integration
- command execution
- true-color support
- unicode characters support (not just ANSI)
- that's it lmfao

## requirements

see [REQUIREMENT.md](REQUIREMENT.md).

## building

```bash
git clone https://github.com/amazonalexa3113-fr/fisterm.git
cd fisterm

cmake -S . -B build
cmake --build build -j$(nproc)

# or maybe use the file that compiles the app for you
./plscompCMake
```

## running

```bash
./build/fisterm
```

## project structure

```text
fisterm/
├── src/
├── include/
├── CMakeLists.txt
├── README.md
└── REQUIREMENT.md
```

## license

[add your chosen license here]
