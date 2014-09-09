redis.invoke('del', 'hello');
redis.invoke('set', 'hello', 123);
console.log(redis.invoke('get', 'hello'));
// console.log(redis.invoke('GET', 'hello'));
// for(var i=0;i<100000;i++)
// redis.invoke('GET', 'hello')

function bench(desc, fn) {
	var now = +new Date;
	fn();
	var dt = (+new Date-now)/1000;
	console.log(desc+' time: '+dt.toFixed(3)+' sec speed: '+(1000000/dt)+' ops/sec');
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

bench('get 1', function(){
	for(var i=0;i<1000000;i++) {
		redis.invoke('get', 'hello');
	}
});
bench('get 2', function(){
	for(var i=0;i<1000000;i++) {
		redis.invoke('get', 'hello');
	}
});
bench('get 3', function(){
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
