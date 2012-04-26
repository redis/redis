local changed_ts_key = KEYS[1] .. '***'
local last_ms = tonumber(redis.call('get', changed_ts_key))
local now_ts = redis.call('time')
local now_ms = (1000 * now_ts[1]) + now_ts[2]/1000
local dt_ms = 0
local decay_ms = 1000
if last_ms == nil then
   dt_ms = 0
else
   dt_ms = now_ms - last_ms
end

local last_updated_val = tonumber(redis.call('get', KEYS[1]))
if last_updated_val == nil then
   last_updated_val = 0      
end

local increment = 1 - math.floor( dt_ms / decay_ms )
redis.call('set', changed_ts_key, now_ms)
local new_val = last_updated_val + increment
if new_val < 0 then
   return {last_updated_val, dt_ms, increment, new_val, redis.call('incrby', KEYS[1], 1 - last_updated_val)}
else
   return {last_updated_val, dt_ms, increment, new_val, redis.call('incrby', KEYS[1], increment)}
end
