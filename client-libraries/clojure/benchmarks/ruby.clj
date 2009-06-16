(ns benchmarks.ruby
  (:require redis))

(dotimes [n 4]
  (redis/with-server 
   {:db 15}
   (redis/set "foo" "The first line we sent to the server is some text")
   (time 
    (dotimes [i 20000]
      (let [key (str "key" i)]
        (redis/set key "The first line we sent to the server is some text")
        (redis/get "foo"))))))


;(redis/with-server 
; {}
; (redis/set "foo" "The first line we sent to the server is some text")
; (time 
;  (dotimes [i 20000]
;    (let [key (str "push_trim" i)]
;      (redis/lpush key i)
;      (redis/ltrim key 0 30)))))



