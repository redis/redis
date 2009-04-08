<?php

// poor man's tests

require_once('redis.php');

$r =& new Redis('localhost');
$r->connect();
$r->select_db(9);
$r->flush();
echo $r->ping() . "\n";
echo $r->do_echo('ECHO test') . "\n";
echo "SET aaa " . $r->set('aaa', 'bbb') . "\n";
echo "SETNX aaa " . $r->set('aaa', 'ccc', true) . "\n";
echo "GET aaa " . $r->get('aaa') . "\n";
echo "INCR aaa " . $r->incr('aaa') . "\n";
echo "GET aaa " . $r->get('aaa') . "\n";
echo "INCRBY aaa 3 " . $r->incr('aaa', 2) . "\n";
echo "GET aaa " . $r->get('aaa') . "\n";
echo "DECR aaa " . $r->decr('aaa') . "\n";
echo "GET aaa " . $r->get('aaa') . "\n";
echo "DECRBY aaa 2 " . $r->decr('aaa', 2) . "\n";
echo "GET aaa " . $r->get('aaa') . "\n";
echo "EXISTS aaa " . $r->exists('aaa') . "\n";
echo "EXISTS fsfjslfjkls " . $r->exists('fsfjslfjkls') . "\n";
echo "DELETE aaa " . $r->delete('aaa') . "\n";
echo "EXISTS aaa " . $r->exists('aaa') . "\n";
echo 'SET a1 a2 a3' . $r->set('a1', 'a') . $r->set('a2', 'b') . $r->set('a3', 'c') . "\n";
echo 'KEYS a* ' . print_r($r->keys('a*'), true) . "\n";
echo 'RANDOMKEY ' . $r->randomkey('a*') . "\n";
echo 'RENAME a1 a0 ' . $r->rename('a1', 'a0') . "\n";
echo 'RENAMENX a0 a2 ' . $r->rename('a0', 'a2', true) . "\n";
echo 'RENAMENX a0 a1 ' . $r->rename('a0', 'a1', true) . "\n";

echo 'LPUSH a0 aaa ' . $r->push('a0', 'aaa') . "\n";
echo 'LPUSH a0 bbb ' . $r->push('a0', 'bbb') . "\n";
echo 'RPUSH a0 ccc ' . $r->push('a0', 'ccc', false) . "\n";
echo 'LLEN a0 ' . $r->llen('a0') . "\n";
echo 'LRANGE sdkjhfskdjfh 0 100 ' . print_r($r->lrange('sdkjhfskdjfh', 0, 100), true) . "\n";
echo 'LRANGE a0 0 0 ' . print_r($r->lrange('sdkjhfskdjfh', 0, 0), true) . "\n";
echo 'LRANGE a0 0 100 ' . print_r($r->lrange('a0', 0, 100), true) . "\n";
echo 'LTRIM a0 0 1 ' . $r->ltrim('a0', 0, 1) . "\n";
echo 'LRANGE a0 0 100 ' . print_r($r->lrange('a0', 0, 100), true) . "\n";
echo 'LINDEX a0 0 ' . $r->lindex('a0', 0) . "\n";
echo 'LPUSH a0 bbb ' . $r->push('a0', 'bbb') . "\n";
echo 'LRANGE a0 0 100 ' . print_r($r->lrange('a0', 0, 100), true) . "\n";
echo 'RPOP a0 ' . $r->pop('a0') . "\n";
echo 'LPOP a0 ' . $r->pop('a0', false) . "\n";
echo 'LSET a0 ccc 0 ' . $r->lset('a0', 'ccc', 0) . "\n";
echo 'LRANGE a0 0 100 ' . print_r($r->lrange('a0', 0, 100), true) . "\n";

echo 'SADD s0 aaa ' . $r->sadd('s0', 'aaa') . "\n";
echo 'SADD s0 aaa ' . $r->sadd('s0', 'aaa') . "\n";
echo 'SADD s0 bbb ' . $r->sadd('s0', 'bbb') . "\n";
echo 'SREM s0 bbb ' . $r->srem('s0', 'bbb') . "\n";
echo 'SISMEMBER s0 aaa ' . $r->sismember('s0', 'aaa') . "\n";
echo 'SISMEMBER s0 bbb ' . $r->sismember('s0', 'bbb') . "\n";
echo 'SADD s0 bbb ' . $r->sadd('s0', 'bbb') . "\n";
echo 'SADD s1 bbb ' . $r->sadd('s1', 'bbb') . "\n";
echo 'SADD s1 aaa ' . $r->sadd('s1', 'aaa') . "\n";
echo 'SINTER s0 s1 ' . print_r($r->sinter(array('s0', 's1')), true) . "\n";
echo 'SREM s0 bbb ' . $r->srem('s0', 'bbb') . "\n";
echo 'SINTER s0 s1 ' . print_r($r->sinter(array('s0', 's1')), true) . "\n";
echo 'SMEMBERS s1 ' . print_r($r->smembers('s1'), true) . "\n";

echo 'SELECT 8 ' . $r->select_db(8) . "\n";
echo 'EXISTS s1 ' . $r->exists('s1') . "\n";
if ($r->exists('s1'))
    echo 'SMEMBERS s1 ' . print_r($r->smembers('s1'), true) . "\n";
echo 'SELECT 9 ' . $r->select_db(9) . "\n";
echo 'SMEMBERS s1 ' . print_r($r->smembers('s1'), true) . "\n";
echo 'MOVE s1 8 ' . $r->move('s1', 8) . "\n";
echo 'EXISTS s1 ' . $r->exists('s1') . "\n";
if ($r->exists('s1'))
    echo 'SMEMBERS s1 ' . print_r($r->smembers('s1'), true) . "\n";
echo 'SELECT 8 ' . $r->select_db(8) . "\n";
echo 'SMEMBERS s1 ' . print_r($r->smembers('s1'), true) . "\n";
echo 'SELECT 9 ' . $r->select_db(9) . "\n";

echo 'SAVE ' . $r->save() . "\n";
echo 'BGSAVE ' . $r->save(true) . "\n";
echo 'LASTSAVE ' . $r->lastsave() . "\n";

echo 'INFO ' . print_r($r->info()) . "\n";

?>