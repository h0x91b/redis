#include <v8.h>
#include <hiredis.h>

using namespace v8;

extern "C" {
	#include "redis.h"
}

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

void RedisCall(const v8::FunctionCallbackInfo<v8::Value>& args) {
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
	redisLog(REDIS_NOTICE,"v8_init");
	isolate = Isolate::New();
	Isolate::Scope isolate_scope(isolate);
	HandleScope handle_scope(isolate);
	_global.Reset(isolate, ObjectTemplate::New());
	Local<ObjectTemplate> global = Local<ObjectTemplate>::New(isolate, _global);
	
	global->Set(String::NewFromUtf8(isolate, "hello"), String::NewFromUtf8(isolate, "world"));
	
	Local<ObjectTemplate> Redis = ObjectTemplate::New(isolate);
	Redis->Set(String::NewFromUtf8(isolate, "call"), FunctionTemplate::New(isolate, RedisCall));
	global->Set(String::NewFromUtf8(isolate, "Redis"), Redis);
	global->Set(String::NewFromUtf8(isolate, "_fncCache"), ObjectTemplate::New(isolate));
	
	Handle<Context> v8_context = Context::New(isolate,NULL,global);
	persistent_v8_context.Reset(isolate, v8_context);
	
	if(server.js_client == NULL) {
		server.js_client = createClient(-1);
		server.js_client->flags |= REDIS_LUA_CLIENT;
	}
	
	redisLog(REDIS_NOTICE,"v8_init done");
	return 0;
}

void jsCommand(redisClient *c) {
	redisLog(REDIS_NOTICE, "jsCommand \"%s\"", c->argv[1]->ptr);
	Isolate::Scope isolateScope(isolate);
	HandleScope handle_scope(isolate);
	
	Local<ObjectTemplate> global = Local<ObjectTemplate>::New(isolate, _global);
	
	{
		Isolate::Scope isolateScope(isolate);
		HandleScope handle_scope(isolate);
		Local<Context> v8_context = Local<Context>::New(isolate, persistent_v8_context);
		Context::Scope context_scope(v8_context);
		//dictGenHashFunction
		unsigned int hash = dictGenHashFunction((const char *)c->argv[1]->ptr, strlen((const char *)c->argv[1]->ptr));
		redisLog(REDIS_NOTICE, "hash of js function %d", hash);
		
		Local<Object> window = isolate->GetCurrentContext()->Global();
		Local<Object> fncCache = Local<Object>::Cast(window->Get(String::NewFromUtf8(isolate,"_fncCache")));
		if(fncCache->Has(hash)) {
			redisLog(REDIS_NOTICE, "function in cache");
		} else {
			redisLog(REDIS_NOTICE, "function NOT in cache");
			fncCache->Set(hash, String::NewFromUtf8(isolate, "dfdf"));
		}
	}
	
	Handle<Context> v8_context = Context::New(isolate,NULL,global);
	//persistent context, if need...
	//Local<Context> v8_context = Local<Context>::New(isolate, persistent_v8_context);
	Context::Scope context_scope(v8_context);
	Handle<String> source = String::NewFromUtf8(isolate, (const char *)c->argv[1]->ptr);
	Local<Object> window = isolate->GetCurrentContext()->Global();
	Local<Function> jsFunction = Local<Function>::Cast(window->Get(String::NewFromUtf8(isolate, "Function")));
	Local<Value> args[] = { String::NewFromUtf8(isolate, (const char *)c->argv[1]->ptr) };
	TryCatch trycatch;
	Local<Function> tmpFunc = Local<Function>::Cast(jsFunction->Call(window, 1, args));
	if(tmpFunc.IsEmpty()) {
		Handle<Value> exception = trycatch.Exception();
		String::Utf8Value exception_str(exception);
		redisLog(REDIS_WARNING, "Exception while compiling script: %s", *exception_str);
		addReplyError(c,*exception_str);
		return;
	}
	redisLog(REDIS_NOTICE,"Compiled");
	Local<Value> result = tmpFunc->Call(window, 0, 0);
	
	if(result.IsEmpty()) {
		Handle<Value> exception = trycatch.Exception();
		String::Utf8Value exception_str(exception);
		redisLog(REDIS_WARNING, "Exception while running script: %s", *exception_str);
		addReplyError(c,*exception_str);
		return;
	}
	
	Local<Object> obj = Object::New(isolate);
	obj->Set(String::NewFromUtf8(isolate, "rez"), result);
	
	//stringify result
	Local<Object> JSON = Local<Object>::Cast(window->Get(String::NewFromUtf8(isolate, "JSON")));
	Local<Function> stringify = Local<Function>::Cast(JSON->Get(String::NewFromUtf8(isolate, "stringify")));
	Local<Value> args2[] = { obj };
	Local<String> res = Local<String>::Cast(stringify->Call(JSON, 1, args2));
	String::Utf8Value resStr(res);
	
	redisLog(REDIS_NOTICE, "Script execution result: %s", *resStr);
	addReplyBulkCString(c,*resStr);
	return;
}
