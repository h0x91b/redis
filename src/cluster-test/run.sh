#!/bin/bash

for f in {7000..7002}; do redis-cli -p $f shutdown; done

rm */dump.rdb
rm */nodes.conf

../redis-server ./7000/redis.conf
../redis-server ./7001/redis.conf
../redis-server ./7002/redis.conf
../redis-server ./7003/redis.conf
../redis-server ./7004/redis.conf
../redis-server ./7005/redis.conf
../redis-server ./7006/redis.conf
../redis-server ./7007/redis.conf
../redis-server ./7008/redis.conf
../redis-server ./7009/redis.conf

for f in {7000..7009}; do redis-cli -p $f ping; done

../redis-trib.rb create --replicas 1 --yes 1 127.0.0.1:7000 127.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003 127.0.0.1:7004 127.0.0.1:7005

../redis-trib.rb add-node 127.0.0.1:7006 127.0.0.1:7000
ID=$(redis-cli -p 7006 cluster nodes | grep myself | cut -d" " -f1)
../redis-trib.rb add-node --slave --master-id $ID 127.0.0.1:7007 127.0.0.1:7000
../redis-trib.rb reshard --from all --to $ID --slots 4096 --yes 127.0.0.1:7006


../redis-trib.rb add-node 127.0.0.1:7008 127.0.0.1:7000
ID=$(redis-cli -p 7008 cluster nodes | grep myself | cut -d" " -f1)
../redis-trib.rb add-node --slave --master-id $ID 127.0.0.1:7009 127.0.0.1:7000
../redis-trib.rb reshard --from all --to $ID --slots 3276 --yes 127.0.0.1:7008

