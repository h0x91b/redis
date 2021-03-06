#include "scripting-v8.h"
extern "C" {
    #include "server.h"
    #include "cluster.h"
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
