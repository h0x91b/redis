function registerUser(name, age, email, password) {
    if(redisCall('SISMEMBER', 'SET:USERS', email) == 1) {
        throw new Error('User exists');
    }
    redisCall('HMSET', 'HSET:USER:'+email, 'name', name, 'age', age, 'email', email, 'password', password);
    redisCall('SADD', 'SET:USERS', email);
    return redisCall('SCARD', 'SET:USERS');
}

function getAllUsers() {
    return redisCall('SMEMBERS', 'SET:USERS').map((email) => {
        var user = redisCall('HMGET', 'HSET:USER:'+email, 'name', 'age', 'email', 'tweets');
        return {
            name: user[0],
            age: user[1],
            email: user[2],
            tweets: user[3] || 0
        };
    });
}

function tweet(email, text) {
    redisCall('HINCRBY', 'HSET:USER:'+email, 'tweets', 1);
    var twid = redisCall('INCR', 'INCR:TWEETID');
    var timestamp = +new Date;
    redisCall('HMSET', 'HSET:TWEET:'+twid, 'author', email, 'text', text, 'date', timestamp);
    redisCall('ZADD', 'ZSET:USER:'+email+':TWEETS', timestamp, twid);
    return twid;
}

function deleteTweet(twid) {
    var email = redisCall('HGET', 'HSET:TWEET:'+twid, 'author');
    redisCall('ZREM', 'ZSET:USER:'+email+':TWEETS', twid);
    redisCall('DEL', 'HSET:TWEET:'+twid);
}

function getUserTweets(email) {
    return redisCall('ZREVRANGE', 'ZSET:USER:'+email+':TWEETS', 0, -1).map((twid) => {
        var tmp = redisCall('HGETALL', 'HSET:TWEET:'+twid);
        var tweet = {};
        for(var i=0;i<tmp.length;i+=2) {
            tweet[tmp[i]] = tmp[i+1];
        }
        return tweet;
    })
}

// return registerUser('Ram', 17, 'ram@huy.com', '123');
return getAllUsers();
// return tweet('ram@huy.com', 'I`ll go to kill myself :`(');
// return getUserTweets('ram@huy.com');