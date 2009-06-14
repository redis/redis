

(add-classpath "file:///Users/ragge/Projects/clojure/redis-clojure/redis-clojure.jar")

(ns benchmarks.clojure
  (:use clojure.contrib.pprint)
  (:require redis))

(defstruct benchmark-options
  :host
  :port
  :db
  :clients
  :requests
  :key-size
  :keyspace-size
  :data-size)


(defstruct client
  :id
  :request-times
  :requests-performed
  :requests-per-second)

(defstruct result
  :options
  :clients
  :total-time
  :requests)



(defmacro defbenchmark [name & body]
  (let [benchmark-name (symbol (str name "-benchmark"))]
    `(def ~(with-meta benchmark-name {:benchmark true})
          (fn ~benchmark-name
            [client# options# result#]
            (redis/with-server
             {:host (options# :host)
              :port (options# :port)
              :db   (options# :db)}
             (let [requests# (:requests options#)
                   requests-done# (:requests result#)]
               (loop [requests-performed# 0 request-times# []]
                 (if (>= @requests-done# requests#)
                   (assoc client#
                     :request-times request-times#
                     :requests-performed requests-performed#)
                   (do
                     (let [start# (System/nanoTime)]
                       ~@body
                       (let [end# (System/nanoTime)
                             elapsed# (/ (float (- end# start#)) 1000000.0)]
                         (dosync
                          (commute requests-done# inc))
                         (recur (inc requests-performed#)
                                (conj request-times# elapsed#)))))))))))))

(defbenchmark ping
  (redis/ping))

(defbenchmark get
  (redis/get (str "key-" (rand-int 1000))))

(defbenchmark set
  (redis/set (str "key-" (rand-int 1000)) "blahojga!"))

(defbenchmark exists-set-and-get
  (let [key (str "key-" (rand-int 100))]
    (redis/exists key)
    (redis/set    key "blahongaa!")
    (redis/get    key)))


(def *default-options* (struct-map benchmark-options
                         :host "127.0.0.1"
                         :port 6379
                         :db 15
                         :clients 4
                         :requests 10000))

(defn create-clients [options]
  (for [id (range (:clients options))]
    (agent (struct client id))))

(defn create-result [options clients]
  (let [result (struct result options clients 0 (ref 0))]
    result))


(defn requests-by-ms [clients]
  (let [all-times (apply concat (map #(:request-times (deref %)) clients))
        all-times-in-ms (map #(int (/ % 1)) all-times)]
    (sort
     (reduce
      (fn [m time]
        (if (m time)
          (assoc m time (inc (m time)))
          (assoc m time 1)))
      {} all-times-in-ms))))

(defn report-request-times [clients requests]
  (let [requests-dist (map #(let [perc (* 100 (/ (last %) requests))]
                             (conj % perc)) (requests-by-ms clients))]
    (dorun
     (map #(println (format "%.2f%% < %d ms" (float (last %)) (inc (first %))))
          requests-dist))))

(defn report-client-rps [client]
  (let [{:keys [id requests-performed request-times]} @client]
    (when (< 0 requests-performed)
      (let [total-time (apply + request-times)
            requests-per-second (/ (float requests-performed)
                                   total-time)]
        (println total-time)
        (println (format "Client %d: %f rps" id (float requests-per-second)))))))

(defn report-result [result]
  (let [{:keys [clients options]} result
        name (:name result)
        time (:total-time result)
        time-in-seconds (/ time 1000)
        requests (deref (:requests result)) 
        requests-per-second (/ requests time-in-seconds)
        ]
    (do
      (println (format "====== %s =====\n" name))
      (println (format "   %d requests completed in %f seconds\n" requests time-in-seconds))
      (println (format "   %d parallel clients\n" (:clients options)))
      ;(report-request-times clients requests)
      ;(dorun (map report-client-rps clients))
      (println (format "%f requests per second\n\n" requests-per-second))
      )
    )
  )



(defn run-benchmark [fn options]
  (let [clients (create-clients options)
        result (create-result options clients)
        start (System/nanoTime)]
    (dorun
     (map #(send-off % fn options result) clients))
    (apply await clients)
    (let [elapsed (/ (double (- (System/nanoTime) start)) 1000000.0)]
      (dorun
       (map #(when (agent-errors %)
               (pprint (agent-errors %))) clients))
      (assoc result
        :name (str fn)
        :options options
        :clients clients
        :total-time elapsed))))

(defn find-all-benchmarks [ns]
  (filter #(:benchmark (meta %))
          (vals (ns-map ns))))

(defn run-and-report [fn options]
  (let [result (run-benchmark fn options)]
    (report-result result)))

(defn run-all-benchmarks [ns]
  (let [benchmarks (find-all-benchmarks ns)]
    (dorun
     (map #(run-and-report % *default-options*) benchmarks))))


;(run-all-benchmarks)

;(report-result (run-benchmark ping-benchmark *default-options*))
;(run-benchmark get-benchmark *default-options*)

