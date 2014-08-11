#include <v8.h>
#include <string.h>

using namespace v8;

extern "C" {
	#include "redis.h"
}

Isolate *isolate = NULL;
Persistent<ObjectTemplate> _global;
Persistent<Context> persistent_v8_context;

int v8_init() {
	redisLog(REDIS_NOTICE,"v8_init");
	isolate = Isolate::New();
	Isolate::Scope isolate_scope(isolate);
	HandleScope handle_scope(isolate);
	_global.Reset(isolate, ObjectTemplate::New());
	Local<ObjectTemplate> global = Local<ObjectTemplate>::New(isolate, _global);
	Handle<Context> v8_context = Context::New(isolate,NULL,global);
	persistent_v8_context.Reset(isolate, v8_context);
	
	redisLog(REDIS_NOTICE,"v8_init done");
	return 0;
}

void jsCommand(redisClient *c) {
	redisLog(REDIS_NOTICE, "jsCommand \"%s\"", c->argv[1]->ptr);
	Isolate::Scope isolateScope(isolate);
	HandleScope handle_scope(isolate);
	
	Local<ObjectTemplate> global = Local<ObjectTemplate>::New(isolate, _global);
	Handle<Context> v8_context = Context::New(isolate,NULL,global);
	//persistent context, if need...
	//Local<Context> v8_context = Local<Context>::New(isolate, persistent_v8_context);
	Context::Scope context_scope(v8_context);
	Handle<String> source = String::NewFromUtf8(isolate, (const char *)c->argv[1]->ptr);
	TryCatch trycatch;
	Handle<Script> script = Script::Compile(source, String::NewFromUtf8(isolate, "<redis>"));
	if(script.IsEmpty()){
		Handle<Value> exception = trycatch.Exception();
		String::Utf8Value exception_str(exception);
		redisLog(REDIS_NOTICE, "Exception while compile script: %s", *exception_str);
		addReplyError(c,*exception_str);
		return;
	}
	redisLog(REDIS_NOTICE,"Compiled");
	Local<Value> result = script->Run();
	if(result.IsEmpty()) {
		Handle<Value> exception = trycatch.Exception();
		String::Utf8Value exception_str(exception);
		redisLog(REDIS_NOTICE, "Exception while running script: %s", *exception_str);
		addReplyError(c,*exception_str);
		return;
	}
	
	//stringify result
	Local<Object> window = isolate->GetCurrentContext()->Global();
	Local<Object> JSON = Local<Object>::Cast(window->Get(String::NewFromUtf8(isolate, "JSON")));
	Local<Function> stringify = Local<Function>::Cast(JSON->Get(String::NewFromUtf8(isolate, "stringify")));
	Local<Value> args[] = { result };
	Local<String> res = Local<String>::Cast(stringify->Call(JSON, 1, args));
	String::Utf8Value resStr(res);
	
	redisLog(REDIS_NOTICE, "Script execution result: %s", *resStr);
	addReplyBulkCString(c,*resStr);
	return;
}
