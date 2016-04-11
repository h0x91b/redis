#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

#include <stddef.h>

EXTERNC struct client;
EXTERNC void *zmalloc(size_t size);
EXTERNC void zfree(void *ptr);
EXTERNC void jsEvalCommand(client *c);
EXTERNC void initV8();
EXTERNC void shutdownV8();
