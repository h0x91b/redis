#ifndef SCRIPTING_V8_H
#define SCRIPTING_V8_H

#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

EXTERNC struct client;
EXTERNC void jsEvalCommand(client *c);
EXTERNC void initV8();
EXTERNC void shutdownV8();

#ifdef __cplusplus

    #include "libplatform/libplatform.h"
    #include "v8.h"
    #include "../deps/hiredis/hiredis.h"
    using namespace v8;

#endif
#endif
