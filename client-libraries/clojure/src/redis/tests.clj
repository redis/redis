(ns redis.tests
  (:refer-clojure :exclude [get set keys type sort])
  (:require redis)
  (:use [clojure.contrib.test-is]))


(defn server-fixture [f]
  (redis/with-server
   {:host "127.0.0.1"
    :port 6379
    :db 15}
   ;; String value
   (redis/set "foo" "bar")
   ;; List with three items
   (redis/rpush "list" "one")
   (redis/rpush "list" "two")
   (redis/rpush "list" "three")
   ;; Set with three members
   (redis/sadd "set" "one")
   (redis/sadd "set" "two")
   (redis/sadd "set" "three")
   (f)
   (redis/flushdb)))
                     
(use-fixtures :each server-fixture)

(deftest ping
  (is (= "PONG" (redis/ping))))

(deftest set
  (redis/set "bar" "foo")
  (is (= "foo" (redis/get "bar")))
  (redis/set "foo" "baz")
  (is (= "baz" (redis/get "foo"))))

(deftest get
  (is (= nil (redis/get "bar")))
  (is (= "bar" (redis/get "foo"))))

(deftest getset
  (is (= nil   (redis/getset "bar" "foo")))
  (is (= "foo" (redis/get "bar")))
  (is (= "bar" (redis/getset "foo" "baz")))
  (is (= "baz" (redis/get "foo"))))

(deftest mget
  (is (= [nil] (redis/mget "bar")))
  (redis/set "bar" "baz")
  (redis/set "baz" "buz")
  (is (= ["bar"] (redis/mget "foo")))
  (is (= ["bar" "baz"] (redis/mget "foo" "bar")))
  (is (= ["bar" "baz" "buz"] (redis/mget "foo" "bar" "baz")))
  (is (= ["bar" nil "buz"] (redis/mget "foo" "bra" "baz")))
  )

(deftest setnx
  (is (= true (redis/setnx "bar" "foo")))
  (is (= "foo" (redis/get "bar")))
  (is (= false (redis/setnx "foo" "baz")))
  (is (= "bar" (redis/get "foo"))))

(deftest incr
  (is (= 1 (redis/incr "nonexistent")))
  (is (= 1 (redis/incr "foo")))
  (is (= 2 (redis/incr "foo"))))

(deftest incrby
  (is (= 42 (redis/incrby "nonexistent" 42)))
  (is (= 0 (redis/incrby "foo" 0)))
  (is (= 5 (redis/incrby "foo" 5))))

(deftest decr
  (is (= -1 (redis/decr "nonexistent")))
  (is (= -1 (redis/decr "foo")))
  (is (= -2 (redis/decr "foo"))))

(deftest decrby
  (is (= -42 (redis/decrby "nonexistent" 42)))
  (is (= 0 (redis/decrby "foo" 0)))
  (is (= -5 (redis/decrby "foo" 5))))

(deftest exists
  (is (= true (redis/exists "foo")))
  (is (= false (redis/exists "nonexistent"))))

(deftest del
  (is (= false (redis/del "nonexistent")))
  (is (= true (redis/del "foo")))
  (is (= nil  (redis/get "foo"))))

(deftest type
  (is (= :none (redis/type "nonexistent")))
  (is (= :string (redis/type "foo")))
  (is (= :list (redis/type "list")))
  (is (= :set (redis/type "set"))))

(deftest keys
  (is (= nil (redis/keys "a*")))
  (is (= ["foo"] (redis/keys "f*")))
  (is (= ["foo"] (redis/keys "f?o")))
  (redis/set "fuu" "baz")
  (is (= #{"foo" "fuu"} (clojure.core/set (redis/keys "f*")))))

(deftest randomkey
  (redis/flushdb)
  (redis/set "foo" "bar")
  (is (= "foo" (redis/randomkey)))
  (redis/flushdb)
  (is (= "" (redis/randomkey))))

(deftest rename
  (is (thrown? Exception (redis/rename "foo" "foo")))
  (is (thrown? Exception (redis/rename "nonexistent" "foo")))
  (redis/rename "foo" "bar")
  (is (= "bar" (redis/get "bar")))
  (is (= nil (redis/get "foo")))
  (redis/set "foo" "bar")
  (redis/set "bar" "baz")
  (redis/rename "foo" "bar")
  (is (= "bar" (redis/get "bar")))
  (is (= nil (redis/get "foo")))
  )

(deftest renamenx
  (is (thrown? Exception (redis/renamenx "foo" "foo")))
  (is (thrown? Exception (redis/renamenx "nonexistent" "foo")))
  (is (= true (redis/renamenx "foo" "bar")))
  (is (= "bar" (redis/get "bar")))
  (is (= nil (redis/get "foo")))
  (redis/set "foo" "bar")
  (redis/set "bar" "baz")
  (is (= false (redis/renamenx "foo" "bar")))
  )

(deftest dbsize
  (let [size-before (redis/dbsize)]
    (redis/set "anewkey" "value")
    (let [size-after (redis/dbsize)]
      (is (= size-after
             (+ 1 size-before))))))

(deftest expire
  (is (= true (redis/expire "foo" 1)))
  (Thread/sleep 2000)
  (is (= false (redis/exists "foo")))
  (redis/set "foo" "bar")
  (is (= true (redis/expire "foo" 20)))
  (is (= false (redis/expire "foo" 10)))
  (is (= false (redis/expire "nonexistent" 42)))
  )

(deftest ttl
  (is (= -1 (redis/ttl "nonexistent")))
  (is (= -1 (redis/ttl "foo")))
  (redis/expire "foo" 42)
  (is (< 40 (redis/ttl "foo"))))


;;
;; List commands
;;
(deftest rpush
  (is (thrown? Exception (redis/rpush "foo")))
  (redis/rpush "newlist" "one")
  (is (= 1 (redis/llen "newlist")))
  (is (= "one" (redis/lindex "newlist" 0)))
  (redis/del "newlist")
  (redis/rpush "list" "item")
  (is (= "item" (redis/rpop "list"))))

(deftest lpush
  (is (thrown? Exception (redis/lpush "foo")))
  (redis/lpush "newlist" "item")
  (is (= 1 (redis/llen "newlist")))
  (is (= "item" (redis/lindex "newlist" 0)))
  (redis/lpush "list" "item")
  (is (= "item" (redis/lpop "list"))))

(deftest llen
  (is (thrown? Exception (redis/llen "foo")))
  (is (= 0 (redis/llen "newlist")))
  (is (= 3 (redis/llen "list"))))

(deftest lrange
  (is (thrown? Exception (redis/lrange "foo" 0 1)))
  (is (= nil (redis/lrange "newlist" 0 42)))
  (is (= ["one"] (redis/lrange "list" 0 0)))
  (is (= ["three"] (redis/lrange "list" -1 -1)))
  (is (= ["one" "two"] (redis/lrange "list" 0 1)))
  (is (= ["one" "two" "three"] (redis/lrange "list" 0 2)))
  (is (= ["one" "two" "three"] (redis/lrange "list" 0 42)))
  (is (= [] (redis/lrange "list" 42 0)))
)

;; TBD
(deftest ltrim
  (is (thrown? Exception (redis/ltrim "foo" 0 0))))

(deftest lindex
  (is (thrown? Exception (redis/lindex "foo" 0)))
  (is (= nil (redis/lindex "list" 42)))
  (is (= nil (redis/lindex "list" -4)))
  (is (= "one" (redis/lindex "list" 0)))
  (is (= "three" (redis/lindex "list" 2)))
  (is (= "three" (redis/lindex "list" -1))))

(deftest lset
  (is (thrown? Exception (redis/lset "foo" 0 "bar")))
  (is (thrown? Exception (redis/lset "list" 42 "value")))
  (redis/lset "list" 0 "test")
  (is (= "test" (redis/lindex "list" 0)))
  (redis/lset "list" 2 "test2")
  (is (= "test2" (redis/lindex "list" 2)))
  (redis/lset "list" -1 "test3")
  (is (= "test3" (redis/lindex "list" 2))))


;; TBD
(deftest lrem
  (is (thrown? Exception (redis/lrem "foo" 0 "bar")))
  (is (= 0 (redis/lrem "list" 0 ""))))


(deftest lpop
  (is (thrown? Exception (redis/lpop "foo")))
  (is (= "one" (redis/lpop "list"))))

(deftest rpop
  (is (thrown? Exception (redis/rpop "foo")))
  (is (= "three" (redis/rpop "list"))))

;;
;; Set commands
;;
(deftest sadd
  (is (thrown? Exception (redis/sadd "foo" "bar")))
  (is (= true (redis/sadd "newset" "member")))
  (is (= true (redis/sismember "newset" "member")))
  (is (= false (redis/sadd "set" "two")))
  (is (= true (redis/sadd "set" "four")))
  (is (= true (redis/sismember "set" "four"))))

(deftest srem
  (is (thrown? Exception (redis/srem "foo" "bar")))
  (is (thrown? Exception (redis/srem "newset" "member")))
  (is (= true (redis/srem "set" "two")))
  (is (= false (redis/sismember "set" "two")))
  (is (= false (redis/srem "set" "blahonga"))))

(deftest smove
  (is (thrown? Exception (redis/smove "foo" "set" "one")))
  (is (thrown? Exception (redis/smove "set" "foo" "one")))
  (redis/sadd "set1" "two")
  (is (= false (redis/smove "set" "set1" "four")))
  (is (= #{"two"} (redis/smembers "set1")))
  (is (= true (redis/smove "set" "set1" "one")))
  (is (= #{"one" "two"} (redis/smembers "set1"))))

(deftest scard
  (is (thrown? Exception (redis/scard "foo")))
  (is (= 3 (redis/scard "set"))))

(deftest sismember
  (is (thrown? Exception (redis/sismember "foo" "bar")))
  (is (= false (redis/sismember "set" "blahonga")))
  (is (= true (redis/sismember "set" "two"))))

(deftest sinter
  (is (thrown? Exception (redis/sinter "foo" "set")))
  (is (= #{} (redis/sinter "nonexistent")))
  (redis/sadd "set1" "one")
  (redis/sadd "set1" "four")
  (is (= #{"one" "two" "three"} (redis/sinter "set")))
  (is (= #{"one"} (redis/sinter "set" "set1")))
  (is (= #{} (redis/sinter "set" "set1" "nonexistent"))))

(deftest sinterstore
  (redis/sinterstore "foo" "set")
  (is (= #{"one" "two" "three"} (redis/smembers "foo")))
  (redis/sadd "set1" "one")
  (redis/sadd "set1" "four")
  (redis/sinterstore "newset" "set" "set1")
  (is (= #{"one"} (redis/smembers "newset"))))

(deftest sunion
  (is (thrown? Exception (redis/sunion "foo" "set")))
  (is (= #{} (redis/sunion "nonexistent")))
  (redis/sadd "set1" "one")
  (redis/sadd "set1" "four")
  (is (= #{"one" "two" "three"} (redis/sunion "set")))
  (is (= #{"one" "two" "three" "four"} (redis/sunion "set" "set1")))
  (is (= #{"one" "two" "three" "four"} (redis/sunion "set" "set1" "nonexistent"))))

(deftest sunionstore
  (redis/sunionstore "foo" "set")
  (is (= #{"one" "two" "three"} (redis/smembers "foo")))
  (redis/sadd "set1" "one")
  (redis/sadd "set1" "four")
  (redis/sunionstore "newset" "set" "set1")
  (is (= #{"one" "two" "three" "four"} (redis/smembers "newset"))))

(deftest sdiff
  (is (thrown? Exception (redis/sdiff "foo" "set")))
  (is (= #{} (redis/sdiff "nonexistent")))
  (redis/sadd "set1" "one")
  (redis/sadd "set1" "four")
  (is (= #{"one" "two" "three"} (redis/sdiff "set")))
  (is (= #{"two" "three"} (redis/sdiff "set" "set1")))
  (is (= #{"two" "three"} (redis/sdiff "set" "set1" "nonexistent"))))

(deftest sdiffstore
  (redis/sdiffstore "foo" "set")
  (is (= #{"one" "two" "three"} (redis/smembers "foo")))
  (redis/sadd "set1" "one")
  (redis/sadd "set1" "four")
  (redis/sdiffstore "newset" "set" "set1")
  (is (= #{"two" "three"} (redis/smembers "newset"))))

(deftest smembers
  (is (thrown? Exception (redis/smembers "foo")))
  (is (= #{"one" "two" "three"} (redis/smembers "set"))))


;;
;; Sorting
;;
(deftest sort
  (redis/lpush "ids" 1)
  (redis/lpush "ids" 4)
  (redis/lpush "ids" 2)
  (redis/lpush "ids" 3)
  (redis/set "object_1" "one")
  (redis/set "object_2" "two")
  (redis/set "object_3" "three")
  (redis/set "object_4" "four")
  (redis/set "name_1" "Derek")
  (redis/set "name_2" "Charlie")
  (redis/set "name_3" "Bob")
  (redis/set "name_4" "Alice")

  (is (= ["one" "two" "three"]
         (redis/sort "list")))
  (is (= ["one" "three" "two"]
         (redis/sort "list" :alpha)))
  (is (= ["1" "2" "3" "4"]
         (redis/sort "ids")))
  (is (= ["1" "2" "3" "4"]
         (redis/sort "ids" :asc)))
  (is (= ["4" "3" "2" "1"]
         (redis/sort "ids" :desc)))
  (is (= ["1" "2"]
         (redis/sort "ids" :asc :limit 0 2)))
  (is (= ["4" "3"]
         (redis/sort "ids" :desc :limit 0 2)))
  (is (= ["4" "3" "2" "1"]
         (redis/sort "ids" :by "name_*" :alpha)))
  (is (= ["one" "two" "three" "four"]
         (redis/sort "ids" :get "object_*")))
  (is (= ["one" "two"]
         (redis/sort "ids" :by "name_*" :alpha :limit 0 2 :desc :get "object_*"))))



;;
;; Multiple database handling commands
;;
(deftest select
  (redis/select 0)
  (is (= nil (redis/get "akeythat_probably_doesnotexsistindb0"))))

(deftest flushdb
  (redis/flushdb)
  (is (= 0 (redis/dbsize))))

;;
;; Persistence commands
;;
(deftest save
  (redis/save))

(deftest bgsave
  (redis/bgsave))

(deftest lastsave
  (let [ages-ago (new java.util.Date (long 1))]
    (is (.before ages-ago (redis/lastsave)))))

