# Glos
Native compiled self hosted language

## Examples
Hello, world:
```rust
use std.io

fn main() {
    &stdout << "Hello, world!\n"
}
```

Program that prints numbers from 0 to 99:
```rust
use std.io

fn main() {
    for let i = 0, i < 100, i += 1 {
        &stdout << i << '\n'
    }
}
```

## Quick Start
Install [FASM](https://flatassembler.net/)

```console
$ ./bin/glos com glos.glos
$ ./glos test tests/*
```
