(ns redis.tests.internal
  (:require [redis.internal :as redis])
  (:use [clojure.contrib.test-is])
  (:import [java.io StringReader BufferedReader]))







;;
;; Helpers
;;

(defn- wrap-in-reader
  [#^String s]
  (let [reader (BufferedReader. (StringReader. s))]
    reader))

(defn- read-reply
  [#^String s]
  (redis/read-reply (wrap-in-reader s)))


;;
;; Command generation
;;
(deftest inline-command
  (is (= "FOO\r\n"
         (redis/inline-command "FOO")))
  (is (= "FOO bar\r\n"
         (redis/inline-command "FOO" "bar")))
  (is (= "FOO bar baz\r\n"
         (redis/inline-command "FOO" "bar" "baz"))))

(deftest bulk-command
  (is (= "FOO 3\r\nbar\r\n"
         (redis/bulk-command "FOO" "bar")))
  (is (= "SET foo 3\r\nbar\r\n"
         (redis/bulk-command "SET" "foo" "bar")))
  (is (= "SET foo bar 3\r\nbaz\r\n"
         (redis/bulk-command "SET" "foo" "bar" "baz"))))

(deftest sort-command
  (is (= "SORT key\r\n"
         (redis/sort-command "SORT" "key")))
  (is (= "SORT key BY pattern\r\n"
         (redis/sort-command "SORT" "key" :by "pattern")))
  (is (= "SORT key LIMIT 0 10\r\n"
         (redis/sort-command "SORT" "key" :limit 0 10)))
  (is (= "SORT key ASC\r\n"
         (redis/sort-command "SORT" "key" :asc)))
  (is (= "SORT key DESC\r\n"
         (redis/sort-command "SORT" "key" :desc)))
  (is (= "SORT key ALPHA\r\n"
         (redis/sort-command "SORT" "key" :alpha)))
  (is (= "SORT key GET object_* GET object2_*\r\n"
         (redis/sort-command "SORT" "key" :get "object_*" :get "object2_*")))
  (is (= "SORT key BY weight_* LIMIT 0 10 GET object_* ALPHA DESC\r\n"
         (redis/sort-command "SORT" "key"
                             :by "weight_*"
                             :limit 0 10
                             :get "object_*"
                             :alpha
                             :desc))))


;;
;; Reply parsing
;;
(deftest read-crlf
  (is (thrown? Exception
               (redis/read-crlf (wrap-in-reader "\n"))))
  (is (thrown? Exception
               (redis/read-crlf (wrap-in-reader ""))))
  (is (thrown? Exception
               (redis/read-crlf (wrap-in-reader "\r1"))))
  (is (= nil
         (redis/read-crlf (wrap-in-reader "\r\n")))))

;; (deftest read-newline-crlf
;;   (is (thrown? Exception
;;                (redis/read-line-crlf (wrap-in-reader "")))))

;;
;; Reply parsing
;;
(deftest reply
  (is (thrown? Exception
               (read-reply "")))
  (is (thrown? Exception
               (read-reply "\r\n"))))


(deftest error-reply
  (is (thrown?
         Exception 
         (read-reply "-\r\n")))
  (is (thrown-with-msg?
         Exception #".*Test"
         (read-reply "-Test\r\n"))))

(deftest simple-reply
  (is (thrown? Exception
               (read-reply "+")))
  (is (= ""
         (read-reply "+\r\n")))
  (is (= "foobar"
         (read-reply "+foobar\r\n"))))

(deftest integer-reply
  (is (thrown? Exception
               (read-reply ":\r\n")))
  (is (= 0
         (read-reply ":0\r\n")))
  (is (= 42
         (read-reply ":42\r\n")))
  (is (= 42
         (read-reply ":  42  \r\n")))
  (is (= 429348754
         (read-reply ":429348754\r\n"))))

(deftest bulk-reply
  (is (thrown? Exception
               (read-reply "$\r\n")))
  (is (thrown? Exception
               (read-reply "$2\r\n1\r\n")))
  (is (thrown? Exception
               (read-reply "$3\r\n1\r\n")))
  (is (= nil
         (read-reply "$-1\r\n")))
  (is (= "foobar"
         (read-reply "$6\r\nfoobar\r\n")))
  (is (= "foo\r\nbar"
         (read-reply "$8\r\nfoo\r\nbar\r\n"))))

(deftest multi-bulk-reply
  (is (thrown? Exception
               (read-reply "*1\r\n")))
  (is (thrown? Exception
               (read-reply "*4\r\n:0\r\n:0\r\n:0\r\n")))
  (is (= nil
         (read-reply "*-1\r\n")))
  (is (= [1]
         (read-reply "*1\r\n:1\r\n")))
  (is (= ["foo" "bar"]
         (read-reply "*2\r\n+foo\r\n+bar\r\n")))
  (is (= [1 "foo" "foo\r\nbar"]
         (read-reply "*3\r\n:1\r\n+foo\r\n$8\r\nfoo\r\nbar\r\n"))))






