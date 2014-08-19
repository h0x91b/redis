Redis.invoke('SET', 'Hello', 'World');

return Redis.invoke('GET', 'Hello');