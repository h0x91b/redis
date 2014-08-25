function bench(desc, fn) {
	var now = +new Date;
	fn();
	console.log(desc+' speed: '+(1000000/((+new Date-now)/1000))+' ops/sec');
}

bench('incr', function(){
	for(var i=0;i<1000000;i++) {
		redis.invoke('incr', 'incr');
	}
});

bench('set', function(){
	for(var i=0;i<1000000;i++) {
		redis.invoke('set', 'hello', i);
	}
});

bench('get', function(){
	for(var i=0;i<1000000;i++) {
		redis.invoke('get', 'hello');
	}
});

bench('hmset', function(){
	for(var i=0;i<1000000;i++) {
		redis.invoke('hmset', 'hset', i, i);
	}
});

bench('hget', function(){
	for(var i=0;i<1000000;i++) {
		redis.invoke('hget', 'hset', i);
	}
});
