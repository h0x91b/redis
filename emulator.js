"use strict"
var DB = {};
log('CoreJSApi');
function InitDB(DBCall, httpResolve, httpReject) {
    log('InitDB');
    var procedures = new Map([
        [
            '1%admin/register-procedure', {
                listed: false,
                opts: {},
                name: 'admin/register-procedure',
                version: 1,
                cb: pAdminRegisterProcedure
            }
        ],
        [
            '1%admin/list-procedures', {
                listed: false,
                opts: {},
                name: 'admin/list-procedures',
                version: 1,
                cb: pAdminListProcedures
            }
        ],
        [
            '1%admin/get-stats', {
                listed: false,
                opts: {},
                name: 'admin/get-stats',
                version: 1,
                cb: pAdminGetStats
            }
        ]
    ]);
    var schemeCounter = new Map([
        ['tweetsInSystem', {listed: true, someSettings:{}}],
        ['adminProcedureCalls', {listed: false, someSettings:{}}],
    ]);
    var schemeStat = new Map([
        ['hello', {listed: true}]
    ]);
    
    DB.DBCall = DBCall;
//    var userFunc;
    DB.__adminEval = function __adminEval(context, userCode, req, res){
        "use strict";
        var userFunc = new Function('req', 'res', 'cb', '"use strict"\n' + userCode);
        userFunc(req, res, cb);
        
        function cb(err, data) {
            if(err) {
                httpReject(context, JSON.stringify({Error:err}));
            } else {
                httpResolve(context, JSON.stringify({Error:null, Data: data}));
            }
        }
    };
    DB.__procedureCall = function __procedureCall(context, path, query, body, req, res) {
        var version = 1;
        var tmp, tmp2;
        req.body = null;
        req.query = {
            pretty: false,
            version: 1
        };
        res.status = '200 OK'; //??
        res.headers = []; //??
        if(query) {
            tmp = query.split('&');
            for(var i=0; i<tmp.length;i++) {
                tmp2 = tmp[i].split('=');
                req.query[tmp2[0]] = true;
                if(tmp2[1])
                    req.query[tmp2[0]] = tmp2[1];
            }
        }
        var fullName = req.query.version + '%' + path;
        var procedure = procedures.get(fullName);
        if(body) req.body = JSON.parse(body);
        if(!procedure) {
            httpReject(context, JSON.stringify({Error: 'Procedure "'+fullName+'" not found'}), '404 Not Found');
            return;
        }
        Counter.incr('adminProcedureCalls');
        procedure.cb(req, res, cb);
        function cb(err, data) {
            if(err) {
                httpReject(context, JSON.stringify({Error:err, Data: data}, null, req.query.pretty ? '\t' : ''), res.status, res.headers);
            } else {
                httpResolve(context, JSON.stringify({Error:null, Data: data}, null, req.query.pretty ? '\t' : ''), res.status, res.headers);
            }
        }
    };
    function registerProcedure(opts, name, version, userCode) {
        log('registerProcedure');
        if(arguments.length != 4) {
            log('wrong args');
            throw new Error('Wrong arguments count');
        }
        if(!Number.isInteger(version)) {
            log('bad version');
            throw new Error('Version must be a number');
        }
        var cb = new Function('req', 'res', 'cb', '"use strict"\n' + userCode);
        var p = {
            opts: opts,
            name: name,
            version: parseInt(version, 10),
            listed: true,
            cb: cb,
            code: userCode
        };
        //7%blog/page/comment/replyTo
        var fullName = p.version + '%' + p.name;
        log(fullName);
        if(procedures.has(fullName)) {
            throw new Error('Procedure with such name already exists');
        }
        Object.defineProperty(cb, 'name', {value: fullName});
        procedures.set(fullName, p);
    };
    
    function pAdminRegisterProcedure(req, res, cb) {
        "use strict"
        log('pAdminRegisterProcedure', JSON.stringify(req));
        if(!req.body)
            return cb('Missing POST body');
        if(!req.body.name)
            return cb('Missing name');
        if(!req.body.version || !Number.isInteger(req.body.version))
            return cb('Version must be an Integer');
        if(!req.body.code)
            return cb('Missing code');
        registerProcedure({}, req.body.name, req.body.version, req.body.code);
        cb(null, 'success');
    }
    
    function pAdminListProcedures(req, res, cb) {
        var ret = [];
        for(var [k, v] of procedures) {
            if(!v.listed) continue;
            ret.push({name: v.name, version: v.version, code: v.code});
        }
        cb(null, ret);
    }
    
    function pAdminGetStats(req, res, cb) {
        var counters = ['adminProcedureCalls', 'tweetsInSystem'];
        var index = 0;
        var stats = {
            totals: {}
        };
        next();
        function next() {
            var name = counters[index++];
            log(name, index, counters);
            Counter.get(name, onResp);
            
            function onResp(e, data) {
                if(e) return cb(e);
                stats.totals[name] = data;
                if(index == counters.length) {
                    log('index == counters.length');
                    cb(null, stats);
                } else {
                    next();
                }
            }
        }
    }
    
    function noCallback(e, data) {
        if(e) {
            log(e);
        }
    }
    
    function Counter(name) {
        if(!name) {
            throw new Error('Missing name');
        }
        this.name = name;
        if(!schemeCounter.has(name)) {
            throw new Error('Counter with name `'+name+'` is not defined');
        }
    }
    Counter.load = function(name) {
        return new Counter(name);
    };
    Counter.get = function(name, cb = noCallback) {
        return Counter.incrBy(name, 0, cb);
    };
    Counter.prototype.get = function(cb = noCallback) {
        return this.incrBy(0, cb);
    };
    Counter.set = function(name, num, cb = noCallback) {
        DBCall(cb, 'SET', 'INCR:'+name, num);
        return Counter;
    };
    Counter.prototype.set = function(num, cb = noCallback) {
        DBCall(cb, 'SET', 'INCR:'+this.name, num);
        return this;
    };
    Counter.incr = function(name, cb = noCallback) {
        return Counter.incrBy(name, 1, cb);
    };
    Counter.prototype.incr = function(cb = noCallback) {
        return this.incrBy(1, cb);
    };
    Counter.decr = function(name, cb = noCallback) {
        return Counter.incrBy(name, -1, cb);
    };
    Counter.prototype.decr = function(cb = noCallback) {
        return this.incrBy(-1, cb);
    };
    Counter.incrBy = function(name, num, cb = noCallback) {
        DBCall(cb, 'INCRBY', 'INCR:'+name, num);
        return Counter;
    };
    Counter.prototype.incrBy = function(num, cb = noCallback) {
        DBCall(cb, 'INCRBY', 'INCR:'+this.name, num);
        return this;
    };
    Counter.flush = function(name, cb = noCallback) {
        return Counter.set(name, 0, cb);
    };
    Counter.prototype.flush = function(cb = noCallback) {
        return this.set(0, cb);
    };
    
    function Stat(name) {
        if(!name) {
            throw new Error('Missing name');
        }
        if(!schemeStat.has(name)) {
            throw new Error('Stat with name `'+name+'` is not defined');
        }
        this.name = name;
    }
    Stat.resolutions = [
        [1000, 300], //by second store 5 minutes
        [60000, 240], //by minute store 4 hours
        [300000, 288], //by 5 minutes store 1 day
        [3600000, 168], //by hour store 1 week
        [86400000, 1000000000], //by day store forever
        [86400000*7, 1000000000], //by week store forever
        [86400000*30, 1000000000], //by month store forever
        [86400000*365, 100], //by year store forever
    ];
    Stat.SECOND = 1000;
    Stat.MINUTE = 60 * Stat.SECOND;
    Stat.FIVEMINUTES = Stat.MINUTE * 5;
    Stat.HOUR = Stat.MINUTE * 60;
    Stat.DAY = Stat.HOUR * 24;
    Stat.MONTH = Stat.DAY * 30;
    Stat.YEAR = Stat.DAY * 365;
    Stat.load = function(name) {
        return new Stat(name);
    };
    Stat.prototype.incr = function(cb = noCallback) {
        return this.incrBy(1, cb);
    };
    Stat.prototype.incrBy = function(num, cb = noCallback) {
        var ret = null, self = this;
        var timestamp = +new Date;
        var t, r, done = 0;
        for(var i=0;i<Stat.resolutions.length;i++) {
            r = Stat.resolutions[i];
            t = timestamp - (timestamp % r[0]);
            DBCall(onResp, 'HINCRBY', 'HSET:STAT:'+this.name+':'+r[0], t, num);
        }
        
        function onResp(e, num) {
            if(e) {
                return cb(e);
            }
            if(!ret) ret = num;
            if(++done == Stat.resolutions.length) {
                return cb(null, ret);
            }
        }
        checkIsCleanNeeded();
        function checkIsCleanNeeded() {
            DBCall((e, cnt)=>{
                if(e) return;
                if(cnt < Stat.resolutions[0][1] * 2) return;
                clean();
            }, 'HLEN', 'HSET:STAT:'+self.name+':'+Stat.resolutions[0][0]);
        }
        function clean() {
            for(let i=0;i<Stat.resolutions.length;i++) {
                let s = Stat.resolutions[i];
                DBCall((e,d)=>{
                    if(e) return;
                    if(d.length < s[1]) return;
                    d = d.sort();
                    d = d.slice(0, d.length - s[1]);
                    DBCall(()=>{}, 'HDEL', 'HSET:STAT:'+self.name+':'+s[0], ...d);
                }, 'HKEYS', 'HSET:STAT:'+self.name+':'+s[0]);
            }
        }
        return this;
    };
    Stat.prototype.getByResolution = function(res, cb = noCallback) {
        DBCall(cb, 'HGETALL', 'HSET:STAT:'+this.name+':'+res);
    };
    
    DB.Counter = Counter;
    DB.Stat = Stat;
    //Object.freeze(DB);
    InitDB = null;
}






/////////////EMULATOR//////////////
var DBCall = (function(){
    var storage = new Map();
    
    return function DBCallEmulator(cb, fn, key, ...args){
        console.log('DBCallEmulator', fn, key, args);
        switch(fn.toUpperCase()) {
        case 'INCRBY':
            var val = storage.get(key) || 0;
            val += args[0];
            storage.set(key, val);
            cb(null, val);
            break;
        case 'HINCRBY':
            var hashmap = storage.get(key) || new Map();
            var val = hashmap.get(args[0]) || 0;
            val += args[1];
            hashmap.set(args[0], val);
            storage.set(key, hashmap);
            cb(null, val);
            break;
        case 'HGETALL':
            var ret = [];
            var hashmap = storage.get(key) || new Map();
            for(var [k, v] of hashmap) {
                ret.push([k, v]);
            }
            cb(null, ret);
            break;
        case 'HLEN':
            var hashmap = storage.get(key) || new Map();
            cb(null, hashmap.size);
            break;
        case 'HKEYS':
            var ret = [];
            var hashmap = storage.get(key) || new Map();
            for(var [k, v] of hashmap) {
                ret.push(k);
            }
            cb(null, ret);
            break;
        case 'HDEL':
            var hashmap = storage.get(key) || new Map();
            for(var i=0;i<args.length;i++) {
                hashmap.delete(args[i]);
            }
            storage.set(key, hashmap);
            cb(null, args.length);
            break;
        default:
            cb(fn.toUpperCase()+' is not implemented');
        }
    };
})();
function log(...args) {
    console.log('Native log', ...args);
}
InitDB(DBCall, (...args)=>{console.log('httpResolve', args)}, (...args)=>{console.log('httpReject', args)});

DB.__procedureCall(null, 'admin/register-procedure', 'pretty', '{"name":"hello/world","version":1,"code":"throw new Error(123)"}', {}, {});
DB.__procedureCall(null, 'admin/list-procedures', 'pretty', '{}', {}, {});

var s = new DB.Stat('hello');
setInterval(function(){
    //console.groupCollapsed()
    s.incrBy(1, (e, d)=>{log(e, d)});
    //console.groupEnd();
}, 1000);