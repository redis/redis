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
         loadmodule src/modules/tlsauth/tlsauth.so USER-ATTRIBUTE commonName REQUIRED-ATTRIBUTE O "Redis Test" REQUIRED-ATTRIBUTE OU Users

4. Start Redis, connect using redis-cli with the different certificates and
   observe user identity assigned depending on certificate used:

         redis-cli --tls \
            --cacert tests/tls/ca.crt \
            --cert tests/tls/alice.crt \
            --key tests/tls/alice.key \
            acl whoami

         redis-cli --tls \
            --cacert tests/tls/ca.crt \
            --cert tests/tls/bob.crt \
            --key tests/tls/bob.key \
         acl whoami

## How it works

The TLSAuth module uses information encoded in the client side certificate to
authenticate users without requiring explicit `AUTH` commands.

A user will be authenticated using the client-side certificate when all of
the following conditions are met:

1. User has established a TLS connection and used a client-side certificate.
2. The client-side certificate is valid and has been signed by a trusted
   certificate authority (per `tls-ca-cert-file` configuration).
3. The certificate Subject Distinguished Name (DN) contains a user identity
   attribute, as specified by USER-ATTRIBUTE (default is `CN` or `commonName`).
4. If REQUIRED-ATTRIBUTE configuration keywords are used, the specified
   attributes and values must be part of the DN.

In the example above, we specify two required attributes:

1. The `O (organization)` attribute should match `Redis Test`.
2. The `OU (organizationalUnit)` attribute should match `Users`.

If no required attributes are specified, any certificate is accepted (as long
as it is valid and signed by a trusted certificate authority).

The user identity is extracted from the attribute specified as USER-ATTRIBUTE.
By default, the `CN (commonName)` attribute is used.

The extracted user ID must match an existing ACL user. If the ID corresponds
to a non-existing or disabled user, authentication will fail and the connection
will fall back to the normal authentication flow.

## Configuration

Configuration is provided as extra arguments to the `MODULE LOAD` command or
`loadmodule` configuration directive.

### USER-ATTRIBUTE `attribute name`

Optional; Default: `CN` or `commonName`.

Specifies the name of the Subject Distinguished Name (DN) attribute that holds
the user identity. The attribute name can be specified in short form (e.g.
`cn`) or long form (e.g. `commonName`).

### REQUIRED-ATTRIBUTE `attribute name` `attribute value`

Optional; Maye be listed multiple times; Default: None

Specifies an attribute (name and value) that must be part of the Subject
Distinguished Name (DN). If the DN does not contain the attribute, or
holds a different value, the certificate is not considered for authentication.
