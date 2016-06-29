#include "scripting-v8.h"
extern "C" {
    #include "server.h"
    #include "cluster.h"
    #include "http_parser.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libplatform/libplatform.h"
#include "v8.h"
#include "../deps/hiredis/hiredis.h"

using namespace v8;

v8::Platform* platform;
sds wrapped_script;
Isolate* isolate;

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
 public:
    virtual void* Allocate(size_t length) {
        void* data = AllocateUninitialized(length);
        return data == NULL ? data : memset(data, 0, length);
    }
    virtual void* AllocateUninitialized(size_t length) { return zmalloc(length); }
    virtual void Free(void* data, size_t) { zfree(data); }
};

void redisLog(const v8::FunctionCallbackInfo<v8::Value>& args) {
    serverLog(LL_NOTICE, "V8 redisLog()");
    for (int i = 0; i < args.Length(); i++) {
        v8::HandleScope handle_scope(args.GetIsolate());
        v8::String::Utf8Value str( args[i]->ToString() );
        serverLogRaw(LL_NOTICE, *str);
    }
}

Local<Value> parseResponse(redisReply *reply) {
    EscapableHandleScope scope(isolate);
    Local<Value> resp;
    Local<Array> arr = Array::New(isolate);
    
    switch(reply->type) {
    case REDIS_REPLY_NIL:
        resp = Null(isolate);
        break;
    case REDIS_REPLY_INTEGER:
        resp = Integer::New(isolate, reply->integer);
        break;
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_STRING:
        resp = String::NewFromUtf8(isolate, reply->str, v8::NewStringType::kNormal, reply->len).ToLocalChecked();
        break;
    case REDIS_REPLY_ARRAY:
        for (size_t i=0; i<reply->elements; i++) {
            arr->Set(Integer::New(isolate, i), parseResponse(reply->element[i]));
        }
        resp = arr;
        break;
    default:
        serverLog(LL_WARNING, "Redis rotocol error, unknown type %d\n", reply->type);
    }
    
    return scope.Escape(resp);
}

void redisCall(const v8::FunctionCallbackInfo<v8::Value>& args) {
    //printf("redisCall %d args\n", args.Length());
    HandleScope handle_scope(isolate);
    client *c = server.v8_client;
    sds reply = NULL;
    int reply_len = 0;
    struct redisCommand *cmd;
    static robj **argv = NULL;
    static int argv_size = 0;
    int argc = args.Length();
    redisReader *reader = redisReaderCreate();
    redisReply *redisReaderResponse = NULL;
    v8::Handle<v8::Value> ret_value;
    int call_flags = CMD_CALL_SLOWLOG | CMD_CALL_STATS;
    
    if (argv_size < argc) {
        argv = (robj **)zrealloc(argv,sizeof(robj*)*argc);
        argv_size = argc;
    }
    
    for (int i = 0; i < args.Length(); i++) {
        v8::HandleScope handle_scope(isolate);
        v8::String::Utf8Value str( args[i]->ToString() );
        argv[i] = createStringObject(*str, str.length());
    }
    
    c->argv = argv;
    c->argc = argc;
    
    /* If this is a Redis Cluster node, we need to make sure Lua is not
    * trying to access non-local keys, with the exception of commands
    * received from our master. */
    if (server.cluster_enabled && !(server.v8_caller->flags & CLIENT_MASTER)) {
       /* Duplicate relevant flags in the lua client. */
       c->flags &= ~(CLIENT_READONLY|CLIENT_ASKING);
       c->flags |= server.v8_caller->flags & (CLIENT_READONLY|CLIENT_ASKING);
       if (getNodeByQuery(c,c->cmd,c->argv,c->argc,NULL,NULL) !=
                          server.cluster->myself)
       {
           args.GetReturnValue().Set(Exception::Error(
               v8::String::NewFromUtf8(isolate, "Lua script attempted to access a non local key in a cluster node", v8::NewStringType::kNormal).ToLocalChecked()
           ));
           goto cleanup;
       }
    }
    
    cmd = lookupCommand((sds)argv[0]->ptr);
    
    if (!cmd || ((cmd->arity > 0 && cmd->arity != argc) ||
                   (argc < -cmd->arity)))
    {
        if (cmd) {
            args.GetReturnValue().Set(Exception::Error(
                v8::String::NewFromUtf8(isolate, "Wrong number of args calling Redis command From Lua script", v8::NewStringType::kNormal).ToLocalChecked()
            ));
        }
        else {
            args.GetReturnValue().Set(Exception::Error(
                v8::String::NewFromUtf8(isolate, "Unknown Redis command called from Lua script", v8::NewStringType::kNormal).ToLocalChecked()
            ));
        }
        goto cleanup;
    }
    
    c->cmd = c->lastcmd = cmd;
    
    if (cmd->flags & CMD_NOSCRIPT) {
        args.GetReturnValue().Set(Exception::Error(
            v8::String::NewFromUtf8(isolate, "This Redis command is not allowed from scripts", v8::NewStringType::kNormal).ToLocalChecked()
        ));
        goto cleanup;
    }
    
    call(c, call_flags);
    
    /* Convert the result of the Redis command into a suitable Lua type.
    * The first thing we need is to create a single string from the client
    * output buffers. */
    if (listLength(c->reply) == 0 && c->bufpos < PROTO_REPLY_CHUNK_BYTES) {
       /* This is a fast path for the common case of a reply inside the
        * client static buffer. Don't create an SDS string but just use
        * the client buffer directly. */
        reply_len = c->bufpos;
        c->buf[c->bufpos] = '\0';
        reply = c->buf;
        c->bufpos = 0;
    } else {
        reply_len = c->bufpos;
        reply = sdsnewlen(c->buf,c->bufpos);
        c->bufpos = 0;
        while(listLength(c->reply)) {
            sds o = (sds)listNodeValue(listFirst(c->reply));
            reply_len += sdslen(o);
            reply = sdscatsds(reply,o);
            listDelNode(c->reply,listFirst(c->reply));
        }
    }
    
    //printf("reply: `%s`\n", reply);
    
    redisReaderFeed(reader, reply, reply_len);
    redisReaderGetReply(reader, (void**)&redisReaderResponse);
    
    if(redisReaderResponse->type == REDIS_REPLY_ERROR) {
        serverLog(LL_WARNING, "reply error %s", redisReaderResponse->str);
        args.GetReturnValue().Set(Exception::Error(
            v8::String::NewFromUtf8(isolate, redisReaderResponse->str, v8::NewStringType::kNormal, redisReaderResponse->len).ToLocalChecked()
        ));
        goto cleanup;
    } else {
        ret_value = parseResponse(redisReaderResponse);
    }
    
    args.GetReturnValue().Set(ret_value);
    
cleanup:
    if(redisReaderResponse != NULL)
        freeReplyObject(redisReaderResponse);
    redisReaderFree(reader);
    for (int j = 0; j < c->argc; j++) {
        robj *o = c->argv[j];
        decrRefCount(o);
    }

    if (c->argv != argv) {
        zfree(c->argv);
        argv = NULL;
        argv_size = 0;
    }
    
    if(reply != NULL && reply != c->buf)
        sdsfree(reply);
}

const unsigned short httpPort = 9500;
const unsigned int tcpBacklog = 512;
const size_t MAX_HEADERS = 64;
char neterrbuf[256];
int httpServer;
#define MAX_ACCEPTS_PER_CALL 1000

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
    sds url;
    sds body;
    size_t headerLen;
    HttpHeader **headers;
};

void httpRequestHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void freeHttpClient(HttpContext *c);

void httpWriteResponse(aeEventLoop *el, int fd, void *privdata, int mask) {
    serverLog(LL_VERBOSE, "httpWriteResponse");
    HttpContext *c = (HttpContext*)privdata;
    static const char resp[] = "HTTP/1.1 200 OK\r\nServer: Redis-v8\r\nContent-Type: application\\json\r\nConnection: Keep-Alive\r\nContent-Length: 7\r\n\r\n\"hello\"";
    if(write(fd, resp, sizeof(resp) - 1) == sizeof(resp) - 1) {
        aeDeleteFileEvent(server.el, fd, AE_WRITABLE);
        
        if(c->connection == HttpConnectionClose) {
            freeHttpClient(c);
            return;
        }
        
        if (aeCreateFileEvent(server.el, fd, AE_READABLE,
            httpRequestHandler, (void*)c) == AE_ERR)
        {
            serverLog(LL_WARNING,"aeCreateFileEvent error");
            freeHttpClient(c);
            return;
        }
    }
}

int onHeadersComplete(http_parser *parser) {
    serverLog(LL_VERBOSE,"onHeadersComplete method: %d %d HTTP/%d.%d Content-Length: %llu", parser->method, HTTP_GET, parser->http_major, parser->http_minor, parser->content_length);
    HttpContext *c = (HttpContext*)parser->data;
    c->contentLength = parser->content_length;
    c->method = parser->method;
    return 0;
}

int onMessageComplete(http_parser *parser) {
    serverLog(LL_VERBOSE,"onMessageComplete");
    HttpContext *c = (HttpContext*)parser->data;
    
    serverLog(LL_VERBOSE,"url: %s", c->url);
    if(c->body) {
        serverLog(LL_VERBOSE,"body: %s", c->body);
    }
    
    for(size_t i=0;i<c->headerLen;i++) {
        serverLog(LL_VERBOSE,"Header: %s=%s", c->headers[i]->field, c->headers[i]->value);
    }
    
    aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
    
    if (aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, httpWriteResponse, (void*)c) == AE_ERR)
    {
        aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
        freeHttpClient(c);
    }
    return 0;
}

bool nextValueConnection;

int onHeaderValue(http_parser *parser, const char *at, size_t length) {
    serverLog(LL_VERBOSE,"onHeaderValue `%.*s`", (int)length, at);
    HttpContext *c = (HttpContext*)parser->data;
    if(c->headerLen < MAX_HEADERS) {
        c->headers[c->headerLen]->value = sdsnewlen(at, length);
        c->headerLen++;
    }
    if(nextValueConnection) {
        nextValueConnection = FALSE;
        if(length == 5 && !strncmp(at, "close", 5)) {
            c->connection = HttpConnectionClose;
        }
    }
    return 0;
}

int onHeaderField(http_parser *parser, const char *at, size_t length) {
    serverLog(LL_VERBOSE,"onHeaderField `%.*s`", (int)length, at);
    HttpContext *c = (HttpContext*)parser->data;
    if(c->headerLen < MAX_HEADERS) {
        c->headers[c->headerLen] = (HttpHeader*)zcalloc(sizeof(HttpHeader*));
        c->headers[c->headerLen]->field = sdsnewlen(at, length);
    }
    if(length == 10) {
        if(at[0] != 'C' && at[0] != 'c') return 0;
        if(!strncmp(at+1,"onnection",9)) {
            nextValueConnection = TRUE;
        }
    }
    return 0;
}

int onUrl(http_parser *parser, const char *at, size_t length) {
    serverLog(LL_VERBOSE,"onUrl `%.*s`", (int)length, at);
    HttpContext *c = (HttpContext*)parser->data;
    c->url = sdsnewlen(at, length);
    return 0;
}

int onBody(http_parser *parser, const char *at, size_t length) {
    HttpContext *c = (HttpContext*)parser->data;
    serverLog(LL_VERBOSE,"onBody (content-length: %zu) `%.*s`", c->contentLength, (int)length, at);
    if(!c->body) {
        c->body = sdsempty();
        if(c->contentLength) {
            c->body = sdsMakeRoomFor(c->body, c->contentLength);
        }
    }
    c->body = sdscatlen(c->body, at, length);
    return 0;
}

void httpRequestHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    serverLog(LL_VERBOSE,"httpRequestHandler");
    http_parser_settings settings;
    http_parser_settings_init(&settings);
    settings.on_headers_complete = onHeadersComplete;
    settings.on_url = onUrl;
    settings.on_header_field = onHeaderField;
    settings.on_header_value = onHeaderValue;
    settings.on_body = onBody;
    settings.on_message_complete = onMessageComplete;
    HttpContext *c = (HttpContext*)privdata;
    char buf[4096];
    int nread = read(fd, buf, 4096);
    if(nread == 0) {
        serverLog(LL_VERBOSE, "Client closed connection");
        aeDeleteFileEvent(server.el, fd, AE_READABLE);
        freeHttpClient(c);
        return;
    }
    if(nread < 0) {
        if (errno == EAGAIN) {
            return;
        }
        if(errno == ECONNRESET) {
            serverLog(LL_VERBOSE,"Connection reset by peer");
            aeDeleteFileEvent(server.el, fd, AE_READABLE);
            freeHttpClient(c);
            return;
        }
        serverLog(LL_WARNING,"error read %d %s", errno, strerror(errno));
        aeDeleteFileEvent(server.el, fd, AE_READABLE);
        freeHttpClient(c);
        return;
    }
    serverLog(LL_VERBOSE,"readed %d bytes %.*s", nread, nread, buf);
    nextValueConnection = FALSE;
    int nparsed = http_parser_execute(c->parser, &settings, buf, nread);
    serverLog(LL_VERBOSE,"nparsed %d bytes", nparsed);
}

HttpContext *allocHttpClient(int fd) {
    serverLog(LL_VERBOSE,"allocHttpClient");
    HttpContext *c = (HttpContext*)zcalloc(sizeof(HttpContext));
    c->fd = fd;
    http_parser *parser = (http_parser*)zmalloc(sizeof(http_parser));
    http_parser_init(parser, HTTP_REQUEST);
    parser->data = (void*)c;
    c->parser = parser;
    c->connection = HttpConnectionKeepAlive;
    c->headers = (HttpHeader**)zcalloc(sizeof(HttpHeader*) * MAX_HEADERS);
    return c;
}

void freeHttpClient(HttpContext *c) {
    serverLog(LL_VERBOSE,"freeHttpClient");
    if(c->fd) close(c->fd);
    if(c->parser) zfree(c->parser);
    if(c->body) sdsfree(c->body);
    if(c->url) sdsfree(c->url);
    for(size_t i=0;i<c->headerLen;i++) {
        sdsfree(c->headers[i]->field);
        sdsfree(c->headers[i]->value);
        zfree(c->headers[i]);
    }
    zfree(c->headers);
    zfree(c);
}

void httpAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    serverLog(LL_VERBOSE,"httpAcceptHandler");
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        
        anetNonBlock(NULL,fd);
        anetEnableTcpNoDelay(NULL,fd);
        if(TRUE) {
            anetKeepAlive(NULL,fd,server.tcpkeepalive);
        }
        
        HttpContext *c = allocHttpClient(cfd);
        
        if (aeCreateFileEvent(server.el, cfd, AE_READABLE,
            httpRequestHandler, (void*)c) == AE_ERR)
        {
            freeHttpClient(c);
            return;
        }
        return;
    }
}

void initHttp() {
    serverLog(LL_WARNING,"Init HTTP");
    httpServer = anetTcpServer(neterrbuf, httpPort, NULL, tcpBacklog);
    anetNonBlock(NULL, httpServer);
    if (aeCreateFileEvent(server.el, httpServer, AE_READABLE, httpAcceptHandler, NULL) == AE_ERR)
    {
        serverPanic(
            "Unrecoverable error creating server.ipfd file event.");
    }
    serverLog(LL_WARNING,"HTTP initialized on port %d", httpPort);
}

void initV8() {
    // Initialize V8.
    V8::InitializeICU();
    // V8::InitializeExternalStartupData("");
    ::platform = v8::platform::CreateDefaultPlatform();
    V8::InitializePlatform(::platform);
    V8::Initialize();
    wrapped_script = sdsempty();
    ArrayBufferAllocator allocator;
    Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = &allocator;
    isolate = Isolate::New(create_params);
    serverLog(LL_WARNING,"V8 initialized");
    initHttp();
}

void shutdownV8() {
    isolate->Dispose();
    V8::Dispose();
    V8::ShutdownPlatform();
    delete ::platform;
    sdsfree(wrapped_script);
    serverLog(LL_WARNING,"V8 destroyed");
}

void jsEvalCommand(client *c) {
    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);
    
    server.v8_caller = c;
    server.v8_time_start = mstime();
    
    v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
    global->Set(
        v8::String::NewFromUtf8(isolate, "log", v8::NewStringType::kNormal).ToLocalChecked(),
        v8::FunctionTemplate::New(isolate, redisLog)
    );
    
    global->Set(
        v8::String::NewFromUtf8(isolate, "redisCall", v8::NewStringType::kNormal).ToLocalChecked(),
        v8::FunctionTemplate::New(isolate, redisCall)
    );
    
    Local<Context> context = Context::New(isolate, NULL, global);
    Context::Scope context_scope(context);
    
    //flush previous data
    wrapped_script[0] = '\0';
    sdsupdatelen(wrapped_script);
    
    wrapped_script = sdscatlen(wrapped_script, "JSON.stringify((function wrap(){\n", 33);
    wrapped_script = sdscatlen(wrapped_script, (const char*)c->argv[1]->ptr, sdslen((const sds)c->argv[1]->ptr));
    wrapped_script = sdscatlen(wrapped_script, "\n})(), null, '\\t');", 19);
    
    Local<String> source = String::NewFromUtf8(
        isolate, 
        wrapped_script, 
        NewStringType::kNormal, 
        sdslen((const sds)c->argv[1]->ptr) + 33 + 19
    ).ToLocalChecked();
    TryCatch trycatch(isolate);
    Local<Script> script;
    
    if(!Script::Compile(context, source).ToLocal(&script)) {
        String::Utf8Value error(trycatch.Exception());
        serverLog(LL_WARNING, "JS Compile exception %s\n", *error);
        addReplyErrorFormat(c, "JS Compile exception %s", *error);
        return;
    }
    Local<Value> result;
    if(!script->Run(context).ToLocal(&result)) {
        String::Utf8Value error(trycatch.Exception());
        serverLog(LL_WARNING, "JS Runtime exception %s\n", *error);
        addReplyErrorFormat(c, "JS Runtime exception %s", *error);
    } else {
        String::Utf8Value utf8(result);
        robj *o;
        o = createObject(OBJ_STRING,sdsnew(*utf8));
        addReplyBulk(c,o);
        decrRefCount(o);
    }
}
