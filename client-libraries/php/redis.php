<?php
/*******************************************************************************
 * Redis PHP Bindings - http://code.google.com/p/redis/
 *
 * Copyright 2009 Ludovico Magnocavallo
 * Released under the same license as Redis.
 *
 * Version: 0.1
 *
 * $Revision: 139 $
 * $Date: 2009-03-15 22:59:40 +0100 (Dom, 15 Mar 2009) $
 *
 ******************************************************************************/


class Redis {
    
    var $server;
    var $port;
    var $_sock;
 
    function Redis($host, $port=6379) {
        $this->host = $host;
        $this->port = $port;
    }
    
    function connect() {
        if ($this->_sock)
            return;
        if ($sock = fsockopen($this->host, $this->port, $errno, $errstr)) {
            $this->_sock = $sock;
            return;
        }
        $msg = "Cannot open socket to {$this->host}:{$this->port}";
        if ($errno || $errmsg)
            $msg .= "," . ($errno ? " error $errno" : "") . ($errmsg ? " $errmsg" : "");
        trigger_error("$msg.", E_USER_ERROR);
    }
    
    function disconnect() {
        if ($this->_sock)
            @fclose($this->_sock);
        $this->_sock = null;
    }
    
    function &ping() {
        $this->connect();
        $this->_write("PING\r\n");
        return $this->_simple_response();
    }
    
    function &do_echo($s) {
        $this->connect();
        $this->_write("ECHO " . strlen($s) . "\r\n$s\r\n");
        return $this->_get_value();
    }
    
    function &set($name, $value, $preserve=false) {
        $this->connect();
        $this->_write(
            ($preserve ? 'SETNX' : 'SET') .
            " $name " . strlen($value) . "\r\n$value\r\n"
        );
        return $preserve ? $this->_numeric_response() : $this->_simple_response();
    }
    
    function &get($name) {
        $this->connect();
        $this->_write("GET $name\r\n");
        return $this->_get_value();
    }
    
    function &incr($name, $amount=1) {
        $this->connect();
        if ($amount == 1)
            $this->_write("INCR $name\r\n");
        else
            $this->_write("INCRBY $name $amount\r\n");
        return $this->_numeric_response();
    }
    
    function &decr($name, $amount=1) {
        $this->connect();
        if ($amount == 1)
            $this->_write("DECR $name\r\n");
        else
            $this->_write("DECRBY $name $amount\r\n");
        return $this->_numeric_response();
    }
    
    function &exists($name) {
        $this->connect();
        $this->_write("EXISTS $name\r\n");
        return $this->_numeric_response();
    }
    
    function &delete($name) {
        $this->connect();
        $this->_write("DEL $name\r\n");
        return $this->_numeric_response();
    }
    
    function &keys($pattern) {
        $this->connect();
        $this->_write("KEYS $pattern\r\n");
        return explode(' ', $this->_get_value());
    }
    
    function &randomkey() {
        $this->connect();
        $this->_write("RANDOMKEY\r\n");
        $s =& trim($this->_read());
        $this->_check_for_error($s);
        return $s;
    }
    
    function &rename($src, $dst, $preserve=False) {
        $this->connect();
        if ($preserve) {
            $this->_write("RENAMENX $src $dst\r\n");
            return $this->_numeric_response();
        }
        $this->_write("RENAME $src $dst\r\n");
        return trim($this->_simple_response());
    }
    
    function &push($name, $value, $tail=true) {
        // default is to append the element to the list
        $this->connect();
        $this->_write(
            ($tail ? 'RPUSH' : 'LPUSH') .
            " $name " . strlen($value) . "\r\n$value\r\n"
        );
        return $this->_simple_response();
    }
    
    function &ltrim($name, $start, $end) {
        $this->connect();
        $this->_write("LTRIM $name $start $end\r\n");
        return $this->_simple_response();
    }
    
    function &lindex($name, $index) {
        $this->connect();
        $this->_write("LINDEX $name $index\r\n");
        return $this->_get_value();
    }
    
    function &pop($name, $tail=true) {
        $this->connect();
        $this->_write(
            ($tail ? 'RPOP' : 'LPOP') .
            " $name\r\n"
        );
        return $this->_get_value();
    }
    
    function &llen($name) {
        $this->connect();
        $this->_write("LLEN $name\r\n");
        return $this->_numeric_response();
    }
    
    function &lrange($name, $start, $end) {
        $this->connect();
        $this->_write("LRANGE $name $start $end\r\n");
        return $this->_get_multi();
    }

    function &sort($name, $query=false) {
        $this->connect();
        if ($query === false) {
            $this->_write("SORT $name\r\n");
        } else {
            $this->_write("SORT $name $query\r\n");
        }
        return $this->_get_multi();
    }
    
    function &lset($name, $value, $index) {
        $this->connect();
        $this->_write("LSET $name $index " . strlen($value) . "\r\n$value\r\n");
        return $this->_simple_response();
    }
    
    function &sadd($name, $value) {
        $this->connect();
        $this->_write("SADD $name " . strlen($value) . "\r\n$value\r\n");
        return $this->_numeric_response();
    }
    
    function &srem($name, $value) {
        $this->connect();
        $this->_write("SREM $name " . strlen($value) . "\r\n$value\r\n");
        return $this->_numeric_response();
    }
    
    function &sismember($name, $value) {
        $this->connect();
        $this->_write("SISMEMBER $name " . strlen($value) . "\r\n$value\r\n");
        return $this->_numeric_response();
    }
    
    function &sinter($sets) {
        $this->connect();
        $this->_write('SINTER ' . implode(' ', $sets) . "\r\n");
        return $this->_get_multi();
    }
    
    function &smembers($name) {
        $this->connect();
        $this->_write("SMEMBERS $name\r\n");
        return $this->_get_multi();
    }

    function &scard($name) {
        $this->connect();
        $this->_write("SCARD $name\r\n");
        return $this->_numeric_response();
    }
    
    function &select_db($name) {
        $this->connect();
        $this->_write("SELECT $name\r\n");
        return $this->_simple_response();
    }
    
    function &move($name, $db) {
        $this->connect();
        $this->_write("MOVE $name $db\r\n");
        return $this->_numeric_response();
    }
    
    function &save($background=false) {
        $this->connect();
        $this->_write(($background ? "BGSAVE\r\n" : "SAVE\r\n"));
        return $this->_simple_response();
    }
    
    function &lastsave() {
        $this->connect();
        $this->_write("LASTSAVE\r\n");
        return $this->_numeric_response();
    }
    
    function &_write($s) {
        while ($s) {
            $i = fwrite($this->_sock, $s);
            if ($i == 0)
                break;
            $s = substr($s, $i);
        }
    }
    
    function &_read($len=1024) {
        if ($s = fgets($this->_sock))
            return $s;
        $this->disconnect();
        trigger_error("Cannot read from socket.", E_USER_ERROR);
    }
    
    function _check_for_error(&$s) {
        if (!$s || $s[0] != '-')
            return;
        if (substr($s, 0, 4) == '-ERR')
            trigger_error("Redis error: " . trim(substr($s, 4)), E_USER_ERROR);
        trigger_error("Redis error: " . substr(trim($this->_read()), 5), E_USER_ERROR);
    }
    
    function &_simple_response() {
        $s =& trim($this->_read());
        if ($s[0] == '+')
            return substr($s, 1);
        if ($err =& $this->_check_for_error($s))
            return $err;
        trigger_error("Cannot parse first line '$s' for a simple response", E_USER_ERROR);
    }
    
    function &_numeric_response($allow_negative=True) {
        $s =& trim($this->_read());
        $i = (int)$s;
        if ($i . '' == $s) {
            if (!$allow_negative && $i < 0)
                $this->_check_for_error($s);
            return $i;
        }
        if ($s == 'nil')
            return null;
        trigger_error("Cannot parse '$s' as numeric response.");
    }
    
    function &_get_value() {
        $s =& trim($this->_read());
        if ($s == 'nil')
            return '';
        else if ($s[0] == '-')
            $this->_check_for_error($s);
        $i = (int)$s;
        if ($i . '' != $s)
            trigger_error("Cannot parse '$s' as data length.");
        $buffer = '';
        while ($i > 0) {
            $s = $this->_read();
            $l = strlen($s);
            $i -= $l;
            if ($l > $i) // ending crlf
                $s = rtrim($s);
            $buffer .= $s;
        }
        if ($i == 0)    // let's restore the trailing crlf
            $buffer .= $this->_read();
        return $buffer;
    }
    
    function &_get_multi() {
        $results = array();
        $num =& $this->_numeric_response(false);
        if ($num === false)
            return $results;
        while ($num) {
            $results[] =& $this->_get_value();
            $num -= 1;
        }
        return $results;
    }
    
}   


?>
