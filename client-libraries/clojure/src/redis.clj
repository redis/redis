;(add-classpath "file:///Users/ragge/Projects/clojure/redis-clojure/src/")

(set! *warn-on-reflection* true)

(ns redis
  (:refer-clojure :exclude [get set type keys sort])
  (:use redis.internal))

(defmacro with-server
  "Evaluates body in the context of a new connection to a Redis server
  then closes the connection.

  server-spec is a map with any of the following keys:
    :host     hostname (default \"127.0.0.1\")
    :port     port (default 6379)
    :db       database to use (default 0)"
  [server-spec & body]
  `(with-server* ~server-spec (fn []
                                (do
                                  (redis/select (:db *server*))
                                  ~@body))))


;;
;; Reply conversion functions
;;
(defn int-to-bool
  "Convert integer reply to a boolean value"
  [int]
  (= 1 int))

(defn string-to-keyword
  "Convert a string reply to a keyword"
  [string]
  (keyword string))

(defn string-to-seq
  "Convert a space separated string to a sequence of words"
  [#^String string]
  (if (empty? string)
    nil
    (re-seq #"\S+" string)))

(defn string-to-map
  "Convert strings with format 'key:value\r\n'+ to a map with {key
  value} pairs"
  [#^String string]
  (let [lines (.split string "(\\r\\n|:)")]
    (apply hash-map lines)))

(defn int-to-date
  "Return a Date representation of a UNIX timestamp"
  [int]
  (new java.util.Date (long int)))

(defn seq-to-set
  [sequence]
  (clojure.core/set sequence))

;;
;; Commands
;;
(defcommands
  ;; Connection handling
  (auth        [] :inline)
  (quit        [password] :inline)
  (ping        [] :inline)
  ;; String commands
  (set         [key value] :bulk)
  (get         [key] :inline)
  (getset      [key value] :bulk)
  (setnx       [key value] :bulk int-to-bool)
  (incr        [key] :inline)
  (incrby      [key integer] :inline)
  (decr        [key] :inline)
  (decrby      [key integer] :inline)
  (exists      [key] :inline int-to-bool)
  (mget        [key & keys] :inline)
  (del         [key] :inline int-to-bool)
  ;; Key space commands
  (type        [key] :inline string-to-keyword)
  (keys        [pattern] :inline string-to-seq)
  (randomkey   [] :inline)
  (rename      [oldkey newkey] :inline)
  (renamenx    [oldkey newkey] :inline int-to-bool)
  (dbsize      [] :inline)
  (expire      [key seconds] :inline int-to-bool)
  (ttl         [key] :inline)
  ;; List commands
  (rpush       [key value] :bulk)
  (lpush       [key value] :bulk)
  (llen        [key] :inline)
  (lrange      [key start end] :inline)
  (ltrim       [key start end] :inline)
  (lindex      [key index] :inline)
  (lset        [key index value] :bulk)
  (lrem        [key count value] :bulk)
  (lpop        [key] :inline)
  (rpop        [key] :inline)
  ;; Set commands
  (sadd        [key member] :bulk int-to-bool)
  (srem        [key member] :bulk int-to-bool)
  (smove       [srckey destkey member] :bulk int-to-bool)
  (scard       [key] :inline)
  (sismember   [key member] :bulk int-to-bool)
  (sinter      [key & keys] :inline seq-to-set)
  (sinterstore [destkey key & keys] :inline)
  (sunion      [key & keys] :inline seq-to-set)
  (sunionstore [destkey key & keys] :inline)
  (sdiff       [key & keys] :inline seq-to-set)
  (sdiffstore  [destkey key & keys] :inline)
  (smembers    [key] :inline seq-to-set)
  ;; Multiple database handling commands
  (select      [index] :inline)
  (move        [key dbindex] :inline)
  (flushdb     [] :inline)
  (flushall    [] :inline)
  ;; Sorting
  (sort        [key & options] :sort)
  ;; Persistence
  (save        [] :inline)
  (bgsave      [] :inline)
  (lastsave    [] :inline int-to-date)
  (shutdown    [] :inline)
  (info        [] :inline string-to-map)
  ;;(monitor     [] :inline))
)
