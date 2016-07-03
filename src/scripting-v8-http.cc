#include "scripting-v8-http.h"
extern "C" {
    #include "server.h"
    #include "cluster.h"
    #include "http_parser.h"
    #include "zmalloc.h"
    #include "dict.h"
    #include "sds.h"
}

Global<Function> __adminEval;
Global<Object> DB;
extern v8::Platform* platform;
extern Isolate* isolate;
extern Global<Context> context_;

//Global<Context> context_DB;
char neterrbuf[256];
int httpServer;

const char *COREJSAPI = R"COREAPI(
var DB = {};
function InitDB(redis) {
    var procedures = new Map();
    DB.__adminEval = function __adminEval(userCode){
        "use strict";
        var userFunc = new Function('"use strict"\n' + userCode);
        return JSON.stringify({Error: null, Data: userFunc()});
    };
    DB.__registerProcedure = function(opts, name, version, cb) {
        if(arguments.length != 4) throw new Error('Wrong arguments count');
        if(!Number.isNumber(version)) throw new Error('Version must be a number');
        var p = {
            opts: opts,
            name: name,
            version: parseInt(version, 10),
            cb: cb
        };
        //7%blog/page/comment/replyTo
        var fullName = p.version + '%' + p.name;
        procedures.set(fullName, p);
    };
    //Object.freeze(DB);
    InitDB = null;
}

)COREAPI";

void httpWriteResponse(aeEventLoop *el, int fd, void *privdata, int mask) {
#ifdef DEVELOPER
    serverLog(LL_VERBOSE, "httpWriteResponse");
#endif
    HttpContext *c = (HttpContext*)privdata;
    static sds resp = sdsnewlen("\0", 65536);
    resp[0] = '\0';
    sdsupdatelen(resp);
    
    resp = sdscatprintf(resp, "HTTP/1.1 %s\r\nServer: Redis-v8\r\nContent-Type: application\\json\r\nConnection: Keep-Alive\r\nContent-Length: %zu\r\n\r\n%s", c->status, sdslen(c->response), c->response);
    
    if(write(fd, resp, sdslen(resp)) > 0) {
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
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"onHeadersComplete method: %d %d HTTP/%d.%d Content-Length: %llu", parser->method, HTTP_GET, parser->http_major, parser->http_minor, parser->content_length);
#endif
    HttpContext *c = (HttpContext*)parser->data;
    c->contentLength = parser->content_length;
    c->method = parser->method;
    return 0;
}

void httpResponseReady(HttpContext *c) {
    if (aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, httpWriteResponse, (void*)c) == AE_ERR)
    {
        aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
        freeHttpClient(c);
    }
}

inline void httpPrepareResponse(HttpContext *c, sds status, sds response) {
    if(c->status) sdsfree(c->status);
    c->status = status;
    if(c->response) sdsfree(c->response);
    c->response = response;
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"httpPrepareResponse status %s, response %s", c->status, c->response);
#endif
}

void httpResponse404(HttpContext *c) {
    httpPrepareResponse(c, sdsnew("404 Not Found"), sdsnew("{\"Error\":\"Not found\",\"Code\":404}"));
    httpResponseReady(c);
}

void httpAdminEvalCommand(HttpContext *c) {
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"httpAdminEvalCommand");
#endif
    if(c->method != HTTP_POST) {
        httpPrepareResponse(c, sdsnew("400 Bad request"), sdsnew("{\"Error\":\"400 Bad Request\",\"Code\":400,\"Data\":\"Wrong request method\"}"));
        httpResponseReady(c);
        return;
    }
    
    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);
    
    //same context
    v8::Local<v8::Context> context = v8::Local<v8::Context>::New(isolate, context_);
    v8::Context::Scope context_scope(context);
    
    static bool firstTime = 1;
    
    TryCatch trycatch(isolate);
    if(firstTime) {
        firstTime = 0;
        sds code = sdsnew(COREJSAPI);
        printf("check %s\n", code);
        Local<String> source;
        String::NewFromUtf8(
                                                   isolate,
                                                   code,
                                                   NewStringType::kNormal,
                                                   sdslen(code)
                                                   ).ToLocal(&source);
        Local<Script> script;
        
        if(!Script::Compile(context, source).ToLocal(&script)) {
            String::Utf8Value error(trycatch.Exception());
            String::Utf8Value sss(source);
            serverLog(LL_WARNING, "JS Compile exception %s\nCode: %.*s\nc->body %s \nsource: %.*s", *error, sdslen(code), code, c->body, sss.length(), *sss);
            httpPrepareResponse(c, sdsnew("503 Service Unavailable"), sdsnew(*error));
            httpResponseReady(c);
            return;
        }
        Local<Value> result;
        if(!script->Run(context).ToLocal(&result)) {
            String::Utf8Value error(trycatch.Exception());
            serverLog(LL_WARNING, "JS Runtime exception %s\n", *error);
            httpPrepareResponse(c, sdsnew("503 Service Unavailable"), sdsnew(*error));
            httpResponseReady(c);
            return;
        }
        
        Local<Value> InitDBVal;
        context->Global()->Get(context, String::NewFromUtf8(isolate, "InitDB", NewStringType::kNormal).ToLocalChecked()).ToLocal(&InitDBVal);
        Local<Function> InitDB = Local<Function>::Cast(InitDBVal);
        
        const int argc = 0;
        Local<Value> argv[argc] = {};
        InitDB->Call(context, context->Global(), argc, argv);
        
        Local<Value> adminEvalVal, DBVal;
        context->Global()->Get(context, String::NewFromUtf8(isolate, "DB", NewStringType::kNormal).ToLocalChecked()).ToLocal(&DBVal);
        Local<Object> DB = Local<Object>::Cast(DBVal);
        ::DB.Reset(isolate, DB);
        
        DB->Get(context, String::NewFromUtf8(isolate, "__adminEval", NewStringType::kNormal).ToLocalChecked()).ToLocal(&adminEvalVal);
        Local<Function> adminEval = Local<Function>::Cast(adminEvalVal);
        
        __adminEval.Reset(isolate, adminEval);
        sdsfree(code);
    }
    
    Local<Function> adminEval = v8::Local<v8::Function>::New(isolate, __adminEval);
    Local<String> jsCode;
    String::NewFromUtf8(isolate, c->body, NewStringType::kNormal, sdslen(c->body)).ToLocal(&jsCode);
    const int argc = 1;
    Local<Value> argv[argc] = {jsCode};
    Local<Value> result;
    if(!adminEval->Call(context, context->Global(), argc, argv).ToLocal(&result)) {
        String::Utf8Value error(trycatch.Exception());
        serverLog(LL_WARNING, "JS Runtime exception %s\n", *error);
        httpPrepareResponse(c, sdsnew("503 Service Unavailable"), sdsnew(*error));
        httpResponseReady(c);
        return;
    }
    
    String::Utf8Value utf8(result);
    
    httpPrepareResponse(c, sdsnew("200 OK"), sdsnewlen(*utf8, utf8.length()));
    httpResponseReady(c);
}

int onMessageComplete(http_parser *parser) {
    HttpContext *c = (HttpContext*)parser->data;
    
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"onMessageComplete");
    if(c->uri) serverLog(LL_VERBOSE,"uri: %s", c->uri);
    if(c->query) serverLog(LL_VERBOSE,"query: %s", c->query);
    if(c->path) serverLog(LL_VERBOSE,"path: %s", c->path);
    if(c->fragment) serverLog(LL_VERBOSE,"fragment: %s", c->fragment);
    if(c->body) serverLog(LL_VERBOSE,"body: %s", c->body);
    
    for(size_t i=0;i<c->headerLen;i++) {
        serverLog(LL_VERBOSE,"Header: %s=%s", c->headers[i]->field, c->headers[i]->value);
    }
#endif
    
    aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
    
    if(!c->path) {
        httpResponse404(c);
        return 0;
    }
    if(sdslen(c->path) == 8 && !strncmp(c->path, "/_f/eval", 8)) {
        httpAdminEvalCommand(c);
        return 0;
    }
    
    httpResponse404(c);
    
    return 0;
}

bool nextValueConnection;

int onHeaderValue(http_parser *parser, const char *at, size_t length) {
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"onHeaderValue `%.*s`", (int)length, at);
#endif
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
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"onHeaderField `%.*s`", (int)length, at);
#endif
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

enum UrlParser {
    UrlParserScheme = 0,
    UrlParserHost = 1,
    UrlParserPort = 2,
    UrlParserPath = 3,
    UrlParserQuery = 4,
    UrlParserFragment = 5,
    UrlParserUser = 6
};

void parseUri(HttpContext *c) {
    http_parser_url parser_url;
    http_parser_url_init(&parser_url);
    
    int result = http_parser_parse_url(c->uri, sdslen(c->uri), 0, &parser_url);
    if (result != 0) {
        serverLog(LL_WARNING,"http_parser_parse_url error");
        if(c->status) sdsfree(c->status);
        c->status = sdsnew("400 Bad Request");
        if(c->response) sdsfree(c->response);
        c->response = sdsnew("{\"Error\":\"400 Bad Request\",\"Code\":400}");
        httpResponseReady(c);
        return;
    }
    
    if((parser_url.field_set & (1 << UrlParserQuery)) != 0) {
        if(c->query) sdsfree(c->query);
        c->query = sdsnewlen(c->uri + parser_url.field_data[UrlParserQuery].off, parser_url.field_data[UrlParserQuery].len);
    }
    
    if((parser_url.field_set & (1 << UrlParserPath)) != 0) {
        if(c->path) sdsfree(c->path);
        c->path = sdsnewlen(c->uri + parser_url.field_data[UrlParserPath].off, parser_url.field_data[UrlParserPath].len);
    }
    
    if((parser_url.field_set & (1 << UrlParserFragment)) != 0) {
        if(c->fragment) sdsfree(c->fragment);
        c->fragment = sdsnewlen(c->uri + parser_url.field_data[UrlParserFragment].off, parser_url.field_data[UrlParserFragment].len);
    }
}

int onUrl(http_parser *parser, const char *at, size_t length) {
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"onUrl `%.*s`", (int)length, at);
#endif
    HttpContext *c = (HttpContext*)parser->data;
    if(c->uri) {
        sdsfree(c->uri);
        c->uri = NULL;
    }
    c->uri = sdsnewlen(at, length);
    parseUri(c);
    return 0;
}

int onBody(http_parser *parser, const char *at, size_t length) {
    HttpContext *c = (HttpContext*)parser->data;
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"onBody (content-length: %zu) `%.*s`", c->contentLength, (int)length, at);
#endif
    //serverLog(LL_WARNING,"onBody (content-length: %zu) %d `%.*s`", c->contentLength, (int)length, (int)length, at);
    if(!c->body) {
        c->body = sdsempty();
        if(c->contentLength) {
            c->body = sdsMakeRoomFor(c->body, c->contentLength);
        }
    }
    if(c->contentLength && sdslen(c->body) == c->contentLength) return 0;
    c->body = sdscatlen(c->body, at, length);
    //    serverLog(LL_WARNING,"onBody cat c->body %s length %d", c->body, sdslen(c->body));
    return 0;
}

void httpRequestHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"httpRequestHandler");
#endif
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
    nextValueConnection = FALSE;
    int nparsed = http_parser_execute(c->parser, &settings, buf, nread);
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"readed %d bytes %.*s", nread, nread, buf);
    serverLog(LL_VERBOSE,"nparsed %d bytes", nparsed);
#endif
}

HttpContext *allocHttpClient(int fd) {
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"allocHttpClient");
#endif
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
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"freeHttpClient");
#endif
    if(c->fd) close(c->fd);
    if(c->parser) zfree(c->parser);
    if(c->body) sdsfree(c->body);
    if(c->uri) sdsfree(c->uri);
    if(c->query) sdsfree(c->query);
    if(c->path) sdsfree(c->path);
    if(c->fragment) sdsfree(c->fragment);
    if(c->status) sdsfree(c->status);
    if(c->response) sdsfree(c->response);
    
    for(size_t i=0;i<c->headerLen;i++) {
        sdsfree(c->headers[i]->field);
        sdsfree(c->headers[i]->value);
        zfree(c->headers[i]);
    }
    zfree(c->headers);
    zfree(c);
}

void httpAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
#ifdef DEVELOPER
    serverLog(LL_VERBOSE,"httpAcceptHandler");
#endif
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
#ifdef DEVELOPER
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
#endif
        
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

void initHttp(Local<Context> *context) {
    serverLog(LL_WARNING,"Init HTTP");
    httpServer = anetTcpServer(neterrbuf, httpPort, NULL, tcpBacklog);
    anetNonBlock(NULL, httpServer);
    if (aeCreateFileEvent(server.el, httpServer, AE_READABLE, httpAcceptHandler, NULL) == AE_ERR)
    {
        serverPanic(
                    "Unrecoverable error creating server.ipfd file event.");
    }
    serverLog(LL_WARNING,"HTTP initialized on port %d", httpPort);
    //context_DB.Reset(isolate, *context);
}