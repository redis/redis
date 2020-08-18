# TLSAuth module

This modules integrates TLS client side certificates with the ACL subsystem,
making it possible to automatically grant users their ACL permissions based on
the client-side certificate used to authenticate.

## Getting started

To get started quickly and see how it works, follow these steps:

1. Run `./utils/gen-test-certs.sh` to have server certs generated.

2. Create user certificates for Alice and Bob. We will use the commonName (CN)
   attribute to store the user identity, and a "Redis Test" organization and
   "Users" organizatinalUnit.

        openssl req \
                 -new -sha256 \
                 -newkey rsa:2048 -nodes -keyout tests/tls/alice.key \
                 -subj '/O=Redis Test/OU=Users/CN=alice' | \
                     openssl x509 \
                         -req -sha256 -CA tests/tls/ca.crt \
                         -CAkey tests/tls/ca.key \
                         -CAserial tests/tls/ca.txt \
                         -CAcreateserial \
                         -days 365 \
                         -out tests/tls/alice.crt

        openssl req \
                 -new -sha256 \
                 -newkey rsa:2048 -nodes -keyout tests/tls/bob.key \
                 -subj '/O=Redis Test/OU=Users/CN=bob' | \
                     openssl x509 \
                         -req -sha256 -CA tests/tls/ca.crt \
                         -CAkey tests/tls/ca.key \
                         -CAserial tests/tls/ca.txt \
                         -CAcreateserial \
                         -days 365 \
                         -out tests/tls/bob.crt

3. Create a Redis configuration with two ACL users and TLS enabled:

         tls-ca-cert-file "tests/tls/ca.crt"
         tls-cert-file "tests/tls/redis.crt"
         tls-key-file "tests/tls/redis.key"
         tls-auth-clients yes
         tls-port 6379
         port 0
         user alice on #93dbc7782132ec0d34e8543805a5f85401044c0e04b6c69d4544cac7d29eb029 ~* +@all
         user bob on #2a31d2ffaa7fe4e35b7fe6c990c02489ebee5a18cf388c3236910c59212e11b3 ~* +@all
         loadmodule src/modules/tlsauth/tlsauth.so attr-user=commonName attr-check=O:"Redis Test" attr-check=OU:Users

4. Start Redis, connect using redis-cli with the different certificates and
   observe user identity assigned depending on certificate used:

         redis-cli --tls --cacert tests/tls/ca.crt --cert tests/tls/alice.crt --key tests/tls/alice.key acl whoami
         "alice"

         redis-cli --tls --cacert tests/tls/ca.crt --cert tests/tls/bob.crt --key tests/tls/bob.key acl whoami
         "bob"

## How it works

The TLSAuth module monitors all incoming connections and inspects the
client-side certificate that was in use.

First, it performs a series of checks on the Subject Distinguished Name to
confirm the certificate should be used to extract user information. These checks
are configured by the `attr-check` arguments, which take the form of
`<attribute-name>:<expected-value>`.

In the example above, we specify two checks:

1. The `O (organization)` attribute should match `Redis Test`.
2. The `OU (organizationalUnit)` attribute should match `Users`.

By default, no checks are performed.

If all checks are successful, the user identity is extracted from the attribute
specified in the `attr-user` attribute. By default, the `CN (commonName)`
attribute is used.

Once the identity of the user has been established, the user is authenticated to
Redis as if an explicit `AUTH` command was issued. For this to succeed, the
following conditions must be met:

1. The user needs to be defined as an ACL user.
2. The user must not be disabled.

