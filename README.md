# libclang-callgraph

libclang-callgraph helps you find the various places a function is called in a given C/C++ codebase.

Consider the following example:

```c
void child1() {}
void child2() {}

void root1()
{
    child1();
    child2();
}

void root2() {
    child1();
}
```

libclang-callgraph outputs the following when asked about `child1()`

```
----path 1----
root1():/home/src/example.c:4
  child1():/home/src/example.c:1
----path 2----
root2():/home/src/example.c:10
  child1():/home/src/example.c:1
----call graph rooted at child1():/home/src/example.c:1----
child1():/home/src/example.c:1
```

This information could come in handy for understanding how and when a function is used.

## Usage

libclang-callgraph requires `libclang` and `pthread` to build. You might need to set `CPATH` in order to `make` successfully.
Once built, the builtin help messages should help you get started.
libclang-callgraph works with `compile_commands.json` which could be obtained through [Bear](https://github.com/rizsotto/Bear) among other means.

Note that for specifying function definitions there is substring matching; `dexer` can match `/long/path/ClangIndexer.cpp`.

## Acknowledgment

`uthash`, included in the project is an excellent hash table library.
This tool is inspired by [clang-callgraph](https://github.com/Vermeille/clang-callgraph).
