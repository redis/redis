start_server {tags {"tls"}} {
    if {$::tls} {
        package require tls

        test {TLS: Not accepting non-TLS connections on a TLS port} {
            set s [redis [srv 0 host] [srv 0 port]]
            catch {$s PING} e
            set e
        } {*I/O error*}

        test {TLS: Verify tls-auth-clients behaves as expected} {
            set s [redis [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {*error*} $e

            r CONFIG SET tls-auth-clients no

            set s [redis [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-auth-clients optional

            set s [redis [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-auth-clients yes

            set s [redis [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {*error*} $e
        }

        test {TLS: Verify tls-protocols behaves as expected} {
            r CONFIG SET tls-protocols TLSv1.2

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-tls1.2 0}]
            catch {$s PING} e
            assert_match {*I/O error*} $e

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-tls1.2 1}]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-protocols ""
        }

        test {TLS: Verify tls-ciphers behaves as expected} {
            r CONFIG SET tls-protocols TLSv1.2
            r CONFIG SET tls-ciphers "DEFAULT:-AES128-SHA256"

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "-ALL:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {*I/O error*} $e

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "-ALL:AES256-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-ciphers "DEFAULT"

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "-ALL:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-protocols ""
            r CONFIG SET tls-ciphers "DEFAULT"
        }

        test {TLS: Verify tls-prefer-server-ciphers behaves as expected} {
            r CONFIG SET tls-protocols TLSv1.2
            r CONFIG SET tls-ciphers "AES128-SHA256:AES256-SHA256"

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "AES256-SHA256:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            assert_equal "AES256-SHA256" [dict get [::tls::status [$s channel]] cipher]

            r CONFIG SET tls-prefer-server-ciphers yes

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "AES256-SHA256:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            assert_equal "AES128-SHA256" [dict get [::tls::status [$s channel]] cipher]

            r CONFIG SET tls-protocols ""
            r CONFIG SET tls-ciphers "DEFAULT"
        }
    }
}
