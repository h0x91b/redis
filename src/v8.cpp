#include <v8.h>
#include <hiredis.h>
#include <stdbool.h>
#include <stdio.h>

using namespace v8;

extern "C" {
	#include "redis.h"
}

#define MULTI_LINE_STRING(...) #__VA_ARGS__
const char * jsCore = MULTI_LINE_STRING(
	(function($root){
		window = $root;
		console = {
			debug: function(msg) { Redis.log(0, msg); },
			info: function(msg) { Redis.log(1, msg); },
			log: function(msg) { Redis.log(2, msg); },
			error: function(msg) { Redis.log(3, msg); },
		};
		console.log('jsCore init done');
	})(this);
);

void v8_run_js(redisClient *c, const char* jsCode, bool isolated);
void v8_restart();
Isolate *isolate = NULL;
Persistent<ObjectTemplate> _global;
Persistent<Context> persistent_v8_context;

Handle<Value> parseResponse(redisReply *rReply) {
	Local<Array> arr;
	switch(rReply->type) {
		case REDIS_REPLY_STATUS:
		case REDIS_REPLY_STRING:
			return String::NewFromUtf8(isolate, rReply->str, String::kNormalString, rReply->len);//parse_response();
		case REDIS_REPLY_INTEGER:
			return Integer::New(isolate, rReply->integer);//parse_response();
		case REDIS_REPLY_ERROR:
			isolate->ThrowException(String::NewFromUtf8(isolate, rReply->str, String::kNormalString, rReply->len));
			return Undefined(isolate);
		case REDIS_REPLY_ARRAY:
			arr = Array::New(isolate);
			for(int n=0;n<rReply->elements;n++) {
				arr->Set(Integer::New(isolate, n), parseResponse(rReply->element[n]));
			}
			return arr;
		default:
			isolate->ThrowException(String::NewFromUtf8(isolate, "Unknown reply type"));
			printf("Unknown reply type: %d\n", rReply->type);
			return Undefined(isolate);
	}
}

void V8RedisLog(const v8::FunctionCallbackInfo<v8::Value>& args) {
	if(args.Length() < 2) {
		isolate->ThrowException(String::NewFromUtf8(isolate, "Wrong number of arguments for V8RedisLog"));
	}
	int level = Local<Integer>::Cast(args[0])->Value();
	String::Utf8Value msg(args[1]);
	redisLog(level, "%s", (char*)*msg);
}

void V8RedisInvoke(const v8::FunctionCallbackInfo<v8::Value>& args) {
	//redisLog(REDIS_NOTICE,"APICall %d arguments", args.Length());
	
	struct redisCommand *cmd;
	redisClient *c = server.js_client;
	sds reply;
	int argc = args.Length();
	static robj **argv = NULL;
	static int argv_size = 0;
	
	argv = (robj**)zmalloc(sizeof(robj*)*argc);
	HandleScope handle_scope(isolate);
	for (int i = 0; i < args.Length(); i++) {
		v8::String::Utf8Value str(args[i]);
		argv[i] = createStringObject((char*)*str,str.length());
	}
	
	c->argv = argv;
	c->argc = argc;
	
	cmd = lookupCommandByCString((sds)argv[0]->ptr);
	if(!cmd || ((cmd->arity > 0 && cmd->arity != argc) || (argc < -cmd->arity))) {
		isolate->ThrowException(String::NewFromUtf8(isolate, "Unknown cmd or wrong number of arguments"));
		//clean there
		c->reply_bytes = 0;

		for (int j = 0; j < c->argc; j++)
			decrRefCount(c->argv[j]);
		zfree(c->argv);
		return;
	}
	
	c->cmd = cmd;
	call(c,REDIS_CALL_SLOWLOG | REDIS_CALL_STATS);
	
	reply = sdsempty();
	if (c->bufpos) {
		reply = sdscatlen(reply,c->buf,c->bufpos);
		c->bufpos = 0;
	}

	while(listLength(c->reply)) {
		robj *o = (robj*)listNodeValue(listFirst(c->reply));
		reply = sdscatlen(reply,o->ptr,strlen((const char*)o->ptr));
		listDelNode(c->reply,listFirst(c->reply));
	}
	
	redisReader *rr = redisReaderCreate();
	redisReaderFeed(rr, reply, sdslen(reply));
	void *aux = NULL;
	redisReaderGetReply(rr, &aux);
	redisReply *rReply = (redisReply*)aux;
	Handle<Value> ret_value = parseResponse(rReply);
	Local<String> v8reply = String::NewFromUtf8(isolate, reply);
	
	freeReplyObject(aux);
	redisReaderFree(rr);

	sdsfree(reply);
	c->reply_bytes = 0;

	for (int j = 0; j < c->argc; j++)
		decrRefCount(c->argv[j]);
	zfree(c->argv);
	args.GetReturnValue().Set(ret_value);
}

int v8_init() {
	redisLog(REDIS_NOTICE,"V8 initialization");
	isolate = Isolate::New();
	Isolate::Scope isolate_scope(isolate);
	HandleScope handle_scope(isolate);
	_global.Reset(isolate, ObjectTemplate::New());
	Local<ObjectTemplate> global = Local<ObjectTemplate>::New(isolate, _global);
	
	global->Set(String::NewFromUtf8(isolate, "hello"), String::NewFromUtf8(isolate, "world"));
	
	Local<ObjectTemplate> Redis = ObjectTemplate::New(isolate);
	Redis->Set(String::NewFromUtf8(isolate, "invoke"), FunctionTemplate::New(isolate, V8RedisInvoke));
	Redis->Set(String::NewFromUtf8(isolate, "log"), FunctionTemplate::New(isolate, V8RedisLog));
	global->Set(String::NewFromUtf8(isolate, "Redis"), Redis);
	
	Handle<Context> v8_context = Context::New(isolate,NULL,global);
	persistent_v8_context.Reset(isolate, v8_context);
	
	if(server.js_client == NULL) {
		server.js_client = createClient(-1);
		server.js_client->flags |= REDIS_LUA_CLIENT;
	}
	
	v8_run_js(NULL, jsCore, FALSE);
	
	redisLog(REDIS_NOTICE,"V8 initialization done");
	return 0;
}

void v8_restart() {
	redisLog(REDIS_NOTICE,"Recreate a V8 subsystem");
	_global.Reset();
	persistent_v8_context.Reset();
	isolate->Dispose();
	v8_init();
}

void v8_run_js(redisClient *c, const char* jsCode, bool isolated) {
	Isolate::Scope isolateScope(isolate);
	HandleScope handle_scope(isolate);
	
	Local<ObjectTemplate> global = Local<ObjectTemplate>::New(isolate, _global);
	
	Handle<Context> v8_context;
	if(isolated) 
		v8_context = Context::New(isolate,NULL,global);
	else
		v8_context = Local<Context>::New(isolate, persistent_v8_context);
	Context::Scope context_scope(v8_context);
	Handle<String> source = String::NewFromUtf8(isolate, jsCode);
	
	Local<Object> window = isolate->GetCurrentContext()->Global();
	Local<Function> jsFunction = Local<Function>::Cast(window->Get(String::NewFromUtf8(isolate, "Function")));
	Local<Value> args[] = { String::NewFromUtf8(isolate, jsCode) };
	TryCatch trycatch;
	Local<Function> tmpFunc = Local<Function>::Cast(jsFunction->Call(window, 1, args));
	if(tmpFunc.IsEmpty()) {
		Handle<Value> exception = trycatch.Exception();
		String::Utf8Value exception_str(exception);
		redisLog(REDIS_WARNING, "Exception while compiling script: %s", *exception_str);
		if(c) addReplyError(c,*exception_str);
		return;
	}
	Local<Value> result = tmpFunc->Call(window, 0, 0);
	
	if(result.IsEmpty()) {
		Handle<Value> exception = trycatch.Exception();
		String::Utf8Value exception_str(exception);
		redisLog(REDIS_WARNING, "Exception while running script: %s", *exception_str);
		if(c) addReplyError(c,*exception_str);
		return;
	}
	
	if(!c) return;
	 
	Local<Object> obj = Object::New(isolate);
	obj->Set(String::NewFromUtf8(isolate, "r"), result);
	
	//stringify result
	Local<Object> JSON = Local<Object>::Cast(window->Get(String::NewFromUtf8(isolate, "JSON")));
	Local<Function> stringify = Local<Function>::Cast(JSON->Get(String::NewFromUtf8(isolate, "stringify")));
	Local<Value> args2[] = { obj, Null(isolate), String::NewFromUtf8(isolate, "\t") };
	Local<String> res = Local<String>::Cast(stringify->Call(JSON, 3, args2));
	String::Utf8Value resStr(res);
	
	addReplyBulkCString(c,*resStr);
}

void jsCommand(redisClient *c) {
	redisLog(REDIS_DEBUG, "jsCommand \"%s\"", c->argv[1]->ptr);
	v8_run_js(c, (const char *)c->argv[1]->ptr, TRUE);
}

void jsCommandPersistent(redisClient *c) {
	redisLog(REDIS_DEBUG, "jsCommandPersistent \"%s\"", c->argv[1]->ptr);
	v8_run_js(c, (const char *)c->argv[1]->ptr, FALSE);
}

void jsLoadFile(redisClient *c) {
	redisLog(REDIS_NOTICE, "jsLoadFile \"%s\"", c->argv[1]->ptr);
	
	Isolate::Scope isolateScope(isolate);
	HandleScope handle_scope(isolate);
	Handle<Context> v8_context;
	Local<ObjectTemplate> global = Local<ObjectTemplate>::New(isolate, _global);
	v8_context = Context::New(isolate,NULL,global);
	Context::Scope context_scope(v8_context);
	
	FILE *f = fopen((const char*)c->argv[1]->ptr,"r");
	if(!f) {
		redisLog(REDIS_WARNING, "jsLoadFile failed");
		addReplyError(c, (char*)"Cannot load file");
		return;
	}
	fseek(f, 0L, SEEK_END);
	size_t sz = ftell(f);
	fseek(f, 0L, SEEK_SET);
	
	char *buf = (char*)zmalloc(sz+1);
	size_t pos = fread(buf, 1, sz, f);
	buf[pos] = 0;
	fclose(f);
	redisLog(LOG_INFO, "code \"%s\"", buf);
	v8_run_js(c, (const char*)buf, FALSE);
	zfree(buf);
}

void jsRestart(redisClient *c) {
	v8_restart();
	addReply(c,shared.ok);
}