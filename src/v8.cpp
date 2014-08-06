#include <v8.h>

int v8_init();

extern "C" {
	#include "redis.h"
	void initV8() {
		v8_init();
	}
}

v8::Isolate *isolate = NULL;

int v8_init() {
	redisLog(REDIS_NOTICE,"v8_init");
	isolate = v8::Isolate::New();
	return 0;
}

void v8_ping() {
}

