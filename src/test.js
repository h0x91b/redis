//const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
console = {
	debug: function(msg) { Redis.log(0, msg); },
	info: function(msg) { Redis.log(1, msg); },
	log: function(msg) { Redis.log(2, msg); },
	error: function(msg) { Redis.log(3, msg); },
}

console.debug('debug');
console.info('info');
console.log('log');
console.error('error');
Redis.invoke('SET', 'Hello', 'World');
return Redis.invoke('GET', 'Hello');