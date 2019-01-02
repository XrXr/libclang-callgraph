/*
Copyright (c) 2019 Alan Wu. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

    3. Neither the name of the copyright holder nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#define _POSIX_C_SOURCE 200809L
#include <clang-c/Index.h>
#include <clang-c/CXCompilationDatabase.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include "uthash.h"

#define BUILTIN_STORAGE_SIZE 30
typedef struct {
    int len;
    int capacity;
    const char **data;
    const char *builtinStorage[BUILTIN_STORAGE_SIZE];
} dynArray;

#define MAX_CALLPATH 300
typedef struct {
    int len;
    dynArray paths[MAX_CALLPATH];
} callpaths_t;

typedef struct functionDecl {
    const char *usr;
    const char *displayName;
    dynArray calls;
    dynArray callers;
    CXTranslationUnit tu;
    UT_hash_handle hh;
    int seen;
} functionDecl_t;

typedef struct processedFile {
    CXTranslationUnit tu;
    const char *filename;
} processedFile_t;

typedef struct parseInfo {
    CXCompileCommands commands;
    int nTotalCommands;
    int startCommandIdx;
    int nCommands;
    int nProcessedFiles;
    processedFile_t *processedFiles;
    functionDecl_t *callgraph;
} parseInfo_t;

typedef struct declInsert {
    CXTranslationUnit tu;
    CXCursor currentDecl;  // which function am I under?
    functionDecl_t **callgraph;
} declInsert_t;


static void initDynArray(dynArray *array) {
    array->data = array->builtinStorage;
    array->capacity = BUILTIN_STORAGE_SIZE;
    array->len = 0;
}

static void arrayAppend(dynArray *array, const char *newElement) {
    const size_t elementSize = sizeof(array->data[0]);

    if (array->len == array->capacity) {
        int newCapacity = array->capacity * 2;
        const char **newBuffer = malloc(newCapacity * elementSize);
        memcpy(newBuffer, array->data, array->len * elementSize);
        array->capacity = newCapacity;
        array->data = newBuffer;
    }
    array->data[array->len] = newElement;
    array->len++;
}

static void deduplicatingInsert(dynArray *array, const char *newElement) {
    for (int i = 0; i < array->len; i++) {
        if (strcmp(array->data[i], newElement) == 0) {
            return;
        }
    }
    arrayAppend(array, newElement);
}

static functionDecl_t *ensureDeclPresent(CXCursor cursor, CXTranslationUnit tu, functionDecl_t **callgraph) {
    CXString usrStringWrapper = clang_getCursorUSR(cursor);
    const char *usr = clang_getCString(usrStringWrapper);
    functionDecl_t *decl;

    HASH_FIND(hh, *callgraph, usr, strlen(usr), decl);
    if (decl) {
        clang_disposeString(usrStringWrapper);
        return decl;
    }

    decl = calloc(1, sizeof *decl);
    initDynArray(&decl->calls);
    initDynArray(&decl->callers);
    decl->usr = usr;
    decl->tu = tu;

    CXString fileNameWrapper, displayNameWrapper = clang_getCursorDisplayName(cursor);
    const char *displayName = clang_getCString(displayNameWrapper);
    CXFile file;
    const char *filename;
    unsigned lineNumber;

    CXSourceLocation location = clang_getCursorLocation(cursor);
    clang_getSpellingLocation(location, &file, &lineNumber, NULL, NULL);
    fileNameWrapper = clang_getFileName(file);
    filename = clang_getCString(fileNameWrapper);

    int nBytes = snprintf(NULL, 0, "%s:%s:%d", displayName, filename, lineNumber);
    nBytes++;
    char *buffer = malloc(nBytes);
    snprintf(buffer, nBytes, "%s:%s:%d", displayName, filename, lineNumber);
    decl->displayName = buffer;

    clang_disposeString(displayNameWrapper);
    clang_disposeString(fileNameWrapper);

    HASH_ADD_KEYPTR(hh, *callgraph, usr, strlen(usr), decl);
    return decl;
}

static void addCallSite(functionDecl_t **callgraph, CXCursor caller, CXCursor callee, CXTranslationUnit tu) {
    if (clang_Cursor_isNull(caller) || clang_Cursor_isNull(callee)) return;

    functionDecl_t *callerDecl = ensureDeclPresent(caller, tu, callgraph);
    functionDecl_t *calleeDecl = ensureDeclPresent(callee, tu, callgraph);

    deduplicatingInsert(&callerDecl->calls, calleeDecl->usr);
    deduplicatingInsert(&calleeDecl->callers, callerDecl->usr);
}

static enum CXChildVisitResult collectCallgraph(CXCursor cursor, CXCursor parent, CXClientData clientData) {
    declInsert_t *insert = (declInsert_t *) clientData;

    enum CXCursorKind cursorType = clang_getCursorKind(cursor);
    switch (cursorType) {
        case CXCursor_CallExpr: {
            addCallSite(insert->callgraph, insert->currentDecl, clang_getCursorReferenced(cursor), insert->tu);
            break;
        }
    case CXCursor_FunctionTemplate:
    case CXCursor_CXXMethod:
    case CXCursor_FunctionDecl:
        insert->currentDecl = cursor;
        break;
    default:
        break;
    }

    return CXChildVisit_Recurse;
}

static void clearSeen(functionDecl_t *callgraph) {
    for (functionDecl_t *decl = callgraph; decl != NULL; decl = decl->hh.next) {
        decl->seen = 0;
    }
}

static void bug(const char *message) {
    fprintf(stderr, "BUG: %s", message);
    exit(100);
}

static void findCallpathsHelper(functionDecl_t *callgraph, const char *functionUsr, dynArray *pathSoFar, callpaths_t *callpathsOut) {
    functionDecl_t *decl;

    HASH_FIND(hh, callgraph, functionUsr, strlen(functionUsr), decl);
    if (!decl) bug("reference to non-existent decl");

    if (decl->seen) return;
    decl->seen = 1;

    arrayAppend(pathSoFar, functionUsr);

    if (decl->callers.len == 0) {
        if (callpathsOut->len >= MAX_CALLPATH) {
            fprintf(stderr, "call path too deep. (can only handle up to %d)\n", MAX_CALLPATH);
            exit(1);
        }
        dynArray *newPath = &callpathsOut->paths[callpathsOut->len];
        initDynArray(newPath);

        for (int i = pathSoFar->len-1; i >= 0; i--) {
            arrayAppend(newPath, pathSoFar->data[i]);
        }
        callpathsOut->len++;
    } else {
        const int preRecursionLength = pathSoFar->len;
        for (int i = 0; i < decl->callers.len; i++) {
            pathSoFar->len = preRecursionLength;
            findCallpathsHelper(callgraph, decl->callers.data[i], pathSoFar, callpathsOut);
        }
    }
}

static void findCallPaths(functionDecl_t *callgraph, const char *functionUsr, callpaths_t *callpathsOut) {
    callpathsOut->len = 0;
    clearSeen(callgraph);
    dynArray pathSoFar;
    initDynArray(&pathSoFar);
    findCallpathsHelper(callgraph, functionUsr, &pathSoFar, callpathsOut);
}

static void printCallGraphRecursive(functionDecl_t *callgraph, const char *functionUsr, int indentationLevel, const char *projectRoot, const int showAll) {
    functionDecl_t *decl;

    HASH_FIND(hh, callgraph, functionUsr, strlen(functionUsr), decl);
    if (!decl) bug("reference to non-existent decl");

    const int display = showAll || strstr(decl->displayName, projectRoot);

    if (display) {
        for (int j = 0; j < indentationLevel * 2; j++) putc(' ', stdout);
        if (decl->seen) {
            printf("%s (recursive)\n", decl->displayName);
        } else {
            printf("%s\n", decl->displayName);
        }
    }

    if (decl->seen) return;
    decl->seen = 1;

    for (int i = 0; i < decl->calls.len; i++) {
        if (showAll) {
            printCallGraphRecursive(callgraph, decl->calls.data[i], indentationLevel + 1, NULL, 1);
        } else {
            printCallGraphRecursive(callgraph, decl->calls.data[i], indentationLevel + display, projectRoot, showAll);
        }
    }
}

static void printCallGraph(functionDecl_t *callgraph, const char *rootUsr, const char *projectRoot, const int showAll) {
    clearSeen(callgraph);
    printCallGraphRecursive(callgraph, rootUsr, 0, projectRoot, showAll);
}

static void* parse(void *arg) {
    parseInfo_t *info = (parseInfo_t *) arg;
    info->callgraph = NULL;
    processedFile_t *processedFiles;
    processedFiles = malloc(sizeof(*processedFiles) * info->nCommands);
    int nProcessedFiles = 0;
    CXIndex index = clang_createIndex(1, 1);

    const int end = info->startCommandIdx + info->nCommands;
    for (unsigned i = info->startCommandIdx; i < end && i < info->nTotalCommands; i++, nProcessedFiles++) {
        const char **args;
        CXCompileCommand command = clang_CompileCommands_getCommand(info->commands, i);

        int nArgs = clang_CompileCommand_getNumArgs(command);
        args = malloc(sizeof(*args) * nArgs);
        for (int i = 0; i < nArgs; i++) {
            args[i] = clang_getCString(clang_CompileCommand_getArg(command, i));
        }

        CXTranslationUnit tu;
        enum CXErrorCode parseResult = clang_parseTranslationUnit2FullArgv(index, NULL, args, nArgs, NULL, 0, 0, &tu);
        if (parseResult != CXError_Success) {
            printf("failed to pasre %s. Error code %d\n", args[nArgs-1], parseResult);
            free(args);
            continue;
        }

        // unsigned nDiagnostics = clang_getNumDiagnostics(tu);
        // if (nDiagnostics) {
        //  int proceed = 1;
        //  for (int i = 0; i < nDiagnostics; i++) {
        //      CXDiagnostic diagnostic = clang_getDiagnostic(tu, i);
        //      enum CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        //      if (severity == CXDiagnostic_Error || severity == CXDiagnostic_Fatal) {
        //          proceed = 0;
        //          CXString diagnosticPrint = clang_formatDiagnostic(diagnostic, 0);
        //          fprintf(stderr, "%s\n", clang_getCString(diagnosticPrint));
        //      }
        //  }
        //  if (!proceed) exit(1);
        // }

        declInsert_t insert = { .tu = tu, .callgraph = &info->callgraph };
        clang_visitChildren(clang_getTranslationUnitCursor(tu), collectCallgraph, &insert);
        clang_suspendTranslationUnit(tu);
        processedFiles[nProcessedFiles].tu = tu;
        processedFiles[nProcessedFiles].filename = clang_getCString(clang_CompileCommand_getFilename(command));

        printf("%s done\n", args[nArgs-1]);
        free(args);
    }
    info->processedFiles = processedFiles;
    info->nProcessedFiles = nProcessedFiles;
    return NULL;
}

enum inputClass {
    inputGood,
    inputBad,
    inputDone
};

// caller must free *filename
enum inputClass readInput(char **filename, int *lineNumber, int *columnNumber) {
    size_t lineLength = 0;
    *filename = NULL;
    if (getline(filename, &lineLength, stdin) < 0) return inputDone;

    char *firstColon = strchr(*filename, ':');
    if (!firstColon) return inputBad;
    *firstColon = 0;
    if (sscanf(firstColon+1, "%d:%d", lineNumber, columnNumber) != 2) return inputBad;

    return inputGood;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        puts("usage: callgraph <path to a project root that contain compile_commands.json> [--show-all]");
        exit(1);
    }
    const int showAll = argc >= 3 && strcmp(argv[2], "--show-all") == 0;
    const char *projectRoot = argv[1];
    CXCompilationDatabase_Error errorCode;
    CXCompilationDatabase database = clang_CompilationDatabase_fromDirectory(projectRoot, &errorCode);
    if (errorCode != CXCompilationDatabase_NoError) {
        fprintf(stderr, "Failed to load compilation database from '%s' (is compile_commands.json present?)\n", projectRoot);
        return 1;
    }

    CXCompileCommands commands = clang_CompilationDatabase_getAllCompileCommands(database);
    unsigned nCommands = clang_CompileCommands_getSize(commands);

    int nThreads;
    const int maxThreads = 10;
    pthread_t threads[maxThreads];
    parseInfo_t results[maxThreads];
    const int commandsPerThread = nCommands / maxThreads + 1;
    for (nThreads = 0; nThreads * commandsPerThread < nCommands; nThreads++) {
        if (nThreads >= maxThreads) bug("thread spawn math is off");
        parseInfo_t *info = &results[nThreads];
        info->commands = commands;
        info->nCommands = commandsPerThread;
        info->nTotalCommands = nCommands;
        info->startCommandIdx = nThreads * commandsPerThread;

        pthread_create(&threads[nThreads], NULL, parse, info);
    }
    for (int i = 0; i < nThreads; i++) {
        pthread_join(threads[i], NULL);
    }

    functionDecl_t *callgraph = results[0].callgraph;

    for (int i = 1; i < nThreads; i++) {
        for (functionDecl_t *decl = results[i].callgraph; decl != NULL;) {
            functionDecl_t *n = decl->hh.next;  // moving an item to a different table changes hh.next.
            functionDecl_t *otherDecl = NULL;
            HASH_FIND(hh, callgraph, decl->usr, strlen(decl->usr), otherDecl);
            if (otherDecl) {
                for (int j = 0; j < decl->callers.len; j++) {
                    const char *usr = decl->callers.data[j];
                    deduplicatingInsert(&otherDecl->callers, usr);
                }

                for (int j = 0; j < decl->calls.len; j++) {
                    const char *usr = decl->calls.data[j];
                    deduplicatingInsert(&otherDecl->calls, usr);
                }
            } else {
                HASH_ADD_KEYPTR(hh, callgraph, decl->usr, strlen(decl->usr), decl);
            }
            decl = n;
        }
    }

    while (1) {
        char *filename;
        int lineNumber, columnNumber;
        printf("> ");

        switch (readInput(&filename, &lineNumber, &columnNumber)) {
        case inputBad:
            puts("Please specify a function definition: <filename>:<line number>:<column number>");
            free(filename);
            continue;
        case inputDone:
            free(filename);
            return 0;
        default:
            break;
        }

        CXTranslationUnit tu;
        const char *mappedFilename;

        int tuFound = 0;
        for (int i = 0; i < nThreads; i++) {
            parseInfo_t *result = &results[i];
            for (int j = 0; j < result->nProcessedFiles; j++) {
                processedFile_t *processedFile = &result->processedFiles[j];
                if (strstr(processedFile->filename, filename)) {
                    tu = processedFile->tu;
                    mappedFilename = processedFile->filename;
                    tuFound = 1;
                    break;
                }
            }
            if (tuFound) break;
        }

        if (!tuFound) {
            printf("failed to map '%s' to a file in the project\n", filename);
            free(filename);
            continue;
        }

        printf("mapped '%s' to '%s'\n", filename, mappedFilename);

        clang_reparseTranslationUnit(tu, 0, NULL, 0);
        CXFile fileOfInterest = clang_getFile(tu, mappedFilename);
        CXSourceLocation location = clang_getLocation(tu, fileOfInterest, lineNumber, columnNumber);
        CXCursor declCursor = clang_getCursor(tu, location);
        declCursor = clang_getCursorDefinition(declCursor);
        if (clang_Cursor_isNull(declCursor)) {
            printf("failed to map %s:%d:%d to a function definition\n", mappedFilename, lineNumber, columnNumber);
            free(filename);
            clang_suspendTranslationUnit(tu);
            continue;
        }
        const char *usr = clang_getCString(clang_getCursorUSR(declCursor));
        printf("usr of specified cursor: %s\n", usr);
        if (strlen(usr) == 0) {
            printf("can't handle empty usr\n");
            free(filename);
            clang_suspendTranslationUnit(tu);
            continue;
        }

        functionDecl_t *decl;
        HASH_FIND(hh, callgraph, usr, strlen(usr), decl);
        if (decl) {
            callpaths_t callpaths = { 0 };
            findCallPaths(callgraph, usr, &callpaths);
            for (int j = 0; j < callpaths.len; j++) {
                printf("----path %d----\n", j+1);

                dynArray *path = &callpaths.paths[j];
                for (int k = 0; k < path->len; k++) {
                    functionDecl_t *callpathEntry;
                    HASH_FIND(hh, callgraph, path->data[k], strlen(path->data[k]), callpathEntry);
                    if (!callpathEntry) bug("callpath has an USR entry that isn't in the graph");
                    for (int l = 0; l < k * 2; l++) putc(' ', stdout);
                    printf("%s\n", callpathEntry->displayName);
                }
            }

            printf("----call graph rooted at %s----\n", decl->displayName);
            printCallGraph(callgraph, usr, projectRoot, showAll);
        } else {
            printf("This function doesn't seem to be used in the project\n");
        }

        free(filename);
        clang_suspendTranslationUnit(tu);
    }
}
