#ifndef SCRIPTING_V8_HTTP_H
#define SCRIPTING_V8_HTTP_H

#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

#include "libplatform/libplatform.h"
#include "v8.h"
#include "../deps/hiredis/hiredis.h"
using namespace v8;

typedef char *sds;
struct http_parser;

const unsigned short httpPort = 9500;
const unsigned int tcpBacklog = 512;
const size_t MAX_HEADERS = 64;
#define MAX_ACCEPTS_PER_CALL 1000
#define DEVELOPER

EXTERNC struct aeEventLoop;

enum HttpConnection {
    HttpConnectionKeepAlive,
    HttpConnectionClose
};

struct HttpHeader {
    sds field;
    sds value;
};

struct HttpContext {
    int fd;
    int method;
    HttpConnection connection;
    http_parser *parser;
    size_t contentLength;
    sds uri;
    //scheme:[//[user:password@]host[:port]][/]path[?query][#fragment]
    // sds scheme;
    // sds user;
    // sds password;
    // sds host;
    // sds port;
    sds path;
    sds query;
    sds fragment;
    sds body;
    size_t headerLen;
    HttpHeader **headers;
    sds status;
    sds response;
};

void httpRequestHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void freeHttpClient(HttpContext *c);

#endif