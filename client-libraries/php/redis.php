<?php
/*******************************************************************************
 * Redis PHP Bindings - http://code.google.com/p/redis/
 *
 * Copyright 2009 Ludovico Magnocavallo
 * Copyright 2009 Salvatore Sanfilippo (ported it to PHP5, fixed some bug)
 * Released under the same license as Redis.
 *
 * Version: 0.1
 *
 * $Revision: 139 $
 * $Date: 2009-03-15 22:59:40 +0100 (Dom, 15 Mar 2009) $
 *
 ******************************************************************************/


class Redis {
    public $server;
    public $port;
    private $_sock;

    public function __construct($host='localhost', $port=6379) {
        $this->host = $host;
        $this->port = $port;
    }
    
    public function connect() {
        if ($this->_sock) return;
        if ($sock = fsockopen($this->host, $this->port, $errno, $errstr)) {
            $this->_sock = $sock;
            return;
        }
        $msg = "Cannot open socket to {$this->host}:{$this->port}";
        if ($errno || $errmsg)
            $msg .= "," . ($errno ? " error $errno" : "") .
                            ($errmsg ? " $errmsg" : "");
        trigger_error("$msg.", E_USER_ERROR);
    }
    
    public function disconnect() {
        if ($this->_sock) @fclose($this->_sock);
        $this->_sock = null;
    }
    
    public function ping() {
        $this->connect();
        $this->write("PING\r\n");
        return $this->get_response();
    }
    
    public function do_echo($s) {
        $this->connect();
        $this->write("ECHO " . strlen($s) . "\r\n$s\r\n");
        return $this->get_response();
    }
    
    public function set($name, $value, $preserve=false) {
        $this->connect();
        $this->write(
            ($preserve ? 'SETNX' : 'SET') .
            " $name " . strlen($value) . "\r\n$value\r\n"
        );
        return $this->get_response();
    }
    
    public function get($name) {
        $this->connect();
        $this->write("GET $name\r\n");
        return $this->get_response();
    }
    
    public function incr($name, $amount=1) {
        $this->connect();
        if ($amount == 1)
            $this->write("INCR $name\r\n");
        else
            $this->write("INCRBY $name $amount\r\n");
        return $this->get_response();
    }
    
    public function decr($name, $amount=1) {
        $this->connect();
        if ($amount == 1)
            $this->write("DECR $name\r\n");
        else
            $this->write("DECRBY $name $amount\r\n");
        return $this->get_response();
    }
    
    public function exists($name) {
        $this->connect();
        $this->write("EXISTS $name\r\n");
        return $this->get_response();
    }
    
    public function delete($name) {
        $this->connect();
        $this->write("DEL $name\r\n");
        return $this->get_response();
    }
    
    public function keys($pattern) {
        $this->connect();
        $this->write("KEYS $pattern\r\n");
        return explode(' ', $this->get_response());
    }
    
    public function randomkey() {
        $this->connect();
        $this->write("RANDOMKEY\r\n");
        return $this->get_response();
    }
    
    public function rename($src, $dst) {
        $this->connect();
        $this->write("RENAME $src $dst\r\n");
        return $this->get_response();
    }

    public function renamenx($src, $dst) {
        $this->connect();
        $this->write("RENAMENX $src $dst\r\n");
        return $this->get_response();
    }
    
    public function expire($name, $time) {
        $this->connect();
        $this->write("EXPIRE $name $time\r\n");
        return $this->get_response();
    }
    
    public function push($name, $value, $tail=true) {
        // default is to append the element to the list
        $this->connect();
        $this->write(
            ($tail ? 'RPUSH' : 'LPUSH') .
            " $name " . strlen($value) . "\r\n$value\r\n"
        );
        return $this->get_response();
    }

    public function lpush($name, $value) {
        return $this->push($name, $value, false);
    }

    public function rpush($name, $value) {
        return $this->push($name, $value, true);
    }

    public function ltrim($name, $start, $end) {
        $this->connect();
        $this->write("LTRIM $name $start $end\r\n");
        return $this->get_response();
    }
    
    public function lindex($name, $index) {
        $this->connect();
        $this->write("LINDEX $name $index\r\n");
        return $this->get_response();
    }
    
    public function pop($name, $tail=true) {
        $this->connect();
        $this->write(
            ($tail ? 'RPOP' : 'LPOP') .
            " $name\r\n"
        );
        return $this->get_response();
    }

    public function lpop($name, $value) {
        return $this->pop($name, $value, false);
    }

    public function rpop($name, $value) {
        return $this->pop($name, $value, true);
    }
    
    public function llen($name) {
        $this->connect();
        $this->write("LLEN $name\r\n");
        return $this->get_response();
    }
    
    public function lrange($name, $start, $end) {
        $this->connect();
        $this->write("LRANGE $name $start $end\r\n");
        return $this->get_response();
    }

    public function sort($name, $query=false) {
        $this->connect();
        $this->write($query == false ? "SORT $name\r\n" : "SORT $name $query\r\n");
        return $this->get_response();
    }
    
    public function lset($name, $value, $index) {
        $this->connect();
        $this->write("LSET $name $index " . strlen($value) . "\r\n$value\r\n");
        return $this->get_response();
    }
    
    public function sadd($name, $value) {
        $this->connect();
        $this->write("SADD $name " . strlen($value) . "\r\n$value\r\n");
        return $this->get_response();
    }
    
    public function srem($name, $value) {
        $this->connect();
        $this->write("SREM $name " . strlen($value) . "\r\n$value\r\n");
        return $this->get_response();
    }
    
    public function sismember($name, $value) {
        $this->connect();
        $this->write("SISMEMBER $name " . strlen($value) . "\r\n$value\r\n");
        return $this->get_response();
    }
    
    public function sinter($sets) {
        $this->connect();
        $this->write('SINTER ' . implode(' ', $sets) . "\r\n");
        return $this->get_response();
    }
    
    public function smembers($name) {
        $this->connect();
        $this->write("SMEMBERS $name\r\n");
        return $this->get_response();
    }

    public function scard($name) {
        $this->connect();
        $this->write("SCARD $name\r\n");
        return $this->get_response();
    }
    
    public function select_db($name) {
        $this->connect();
        $this->write("SELECT $name\r\n");
        return $this->get_response();
    }
    
    public function move($name, $db) {
        $this->connect();
        $this->write("MOVE $name $db\r\n");
        return $this->get_response();
    }
    
    public function save($background=false) {
        $this->connect();
        $this->write(($background ? "BGSAVE\r\n" : "SAVE\r\n"));
        return $this->get_response();
    }
    
    public function bgsave($background=false) {
        return $this->save(true);
    }

    public function lastsave() {
        $this->connect();
        $this->write("LASTSAVE\r\n");
        return $this->get_response();
    }
    
    public function flushdb($all=false) {
        $this->connect();
        $this->write($all ? "FLUSHALL\r\n" : "FLUSHDB\r\n");
        return $this->get_response();
    }

    public function flushall() {
        return $this->flush(true);
    }
    
    public function info() {
        $this->connect();
        $this->write("INFO\r\n");
        $info = array();
        $data =& $this->get_response();
        foreach (explode("\r\n", $data) as $l) {
            if (!$l)
                continue;
            list($k, $v) = explode(':', $l, 2);
            $_v = strpos($v, '.') !== false ? (float)$v : (int)$v;
            $info[$k] = (string)$_v == $v ? $_v : $v;
        }
        return $info;
    }
    
    private function write($s) {
        while ($s) {
            $i = fwrite($this->_sock, $s);
            if ($i == 0) // || $i == strlen($s))
                break;
            $s = substr($s, $i);
        }
    }
    
    private function read($len=1024) {
        if ($s = fgets($this->_sock))
            return $s;
        $this->disconnect();
        trigger_error("Cannot read from socket.", E_USER_ERROR);
    }
    
    private function get_response() {
        $data = trim($this->read());
        $c = $data[0];
        $data = substr($data, 1);
        switch ($c) {
            case '-':
                trigger_error($data, E_USER_ERROR);
                break;
            case '+':
                return $data;
            case ':':
                $i = strpos($data, '.') !== false ? (int)$data : (float)$data;
                if ((string)$i != $data)
                    trigger_error("Cannot convert data '$c$data' to integer", E_USER_ERROR);
                return $i;
            case '$':
                return $this->get_bulk_reply($c . $data);
            case '*':
                $num = (int)$data;
                if ((string)$num != $data)
                    trigger_error("Cannot convert multi-response header '$data' to integer", E_USER_ERROR);
                $result = array();
                for ($i=0; $i<$num; $i++)
                    $result[] =& $this->get_response();
                return $result;
            default:
                trigger_error("Invalid reply type byte: '$c'");
        }
    }
    
    private function get_bulk_reply($data=null) {
        if ($data === null)
            $data = trim($this->read());
        if ($data == '$-1')
            return null;
        $c = $data[0];
        $data = substr($data, 1);
        $bulklen = (int)$data;
        if ((string)$bulklen != $data)
            trigger_error("Cannot convert bulk read header '$c$data' to integer", E_USER_ERROR);
        if ($c != '$')
            trigger_error("Unkown response prefix for '$c$data'", E_USER_ERROR);
        $buffer = '';
        while ($bulklen) {
            $data = fread($this->_sock,$bulklen);
            $bulklen -= strlen($data);
            $buffer .= $data;
        }
        $crlf = fread($this->_sock,2);
        return $buffer;
    }
}

/*
$r = new Redis();
var_dump($r->set("foo","bar"));
var_dump($r->get("foo"));
var_dump($r->info());
*/

?>
