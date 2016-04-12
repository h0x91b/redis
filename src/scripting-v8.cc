#include "scripting-v8.h"
extern "C" {
    #include "server.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libplatform/libplatform.h"
#include "v8.h"

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

void Hello(const v8::FunctionCallbackInfo<v8::Value>& args) {
    bool first = true;
    for (int i = 0; i < args.Length(); i++) {
        v8::HandleScope handle_scope(args.GetIsolate());
        if (first) {
            first = false;
        } else {
            printf(" ");
        }
        v8::String::Utf8Value str( args[i]->ToString() );
        printf("%s", *str);
    }
    printf("\n");
    fflush(stdout);
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
    
    v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
    global->Set(
        v8::String::NewFromUtf8(isolate, "Hello", v8::NewStringType::kNormal).ToLocalChecked(),
        v8::FunctionTemplate::New(isolate, Hello)
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
