# Redis with SSL quickstart guide:

## Building SSL version of Redis
Redis with SSL requires building Redis with a new flag, "BUILD_SSL=yes". This builds a version of Redis that supports SSL. This build flag is used to limit the size of the Redis executable and optimize the code paths if SSL is not required.

### Download the latest version of s2n and openssl to use during the build
Redis with SSL requires the installation of two dependencies, S2N and OpenSSL. These libraries are statically linked into the Redis executable. These files are not provided by default in the Redis source code.
```
cd deps 
./download-ssl-deps.sh 
cd ..
```

### Make Redis that is compatible with SSL
```
BUILD_SSL=yes make
```

### Then run some tests afterwards to make sure everything works
```
BUILD_SSL=yes make test
BUILD_SSL=yes make test-ssl
```

## Creating SSL files needed for testing or production use
Redis with SSL requires 3 files to be created and loaded into each server:
1. A server private key, which will be used to sign requests sent from the server.
2. A server certificate/public key, which will be used to sign requests sent to the server and validate the server authenticity.
3. A DH parameters file, which is used as part of the SSL handshake to reduce computation time.

The preferred way to generate these files would to generate a wildcard *.domain certificate from a trusted CA. However, if you are using Redis in a secure environment, you can generate your own key information.

The below commands will setup all the config values needed to launch a node Redis cluster. Openssl must be installed in your environment.

### Clean up the environment
```
rm -rf ssl
mkdir -p ssl
```

### Create a request config file 
This file is used for creating the certificate authority key and server key. This file should be updated to include your information if being used in production.

```
cat > ssl/ca_config.conf << EndOfMessage
[ req ]
default_bits       = 4096
default_md         = sha512
default_keyfile    = domain.com.key
prompt             = no
encrypt_key        = no
distinguished_name = req_distinguished_name
# distinguished_name
[ req_distinguished_name ]
countryName            = "XX"                     # C=
localityName           = "XXXXX"                 # L=
organizationName       = "My Company"             # O=
organizationalUnitName = "Department"            # OU=
commonName             = "*"           # CN=
emailAddress           = "me@domain.com"          # email
EndOfMessage
```

### Create CA Bundle
```
openssl genrsa -out ssl/ca.key 2048
openssl req -x509 -new -nodes -key ssl/ca.key -sha256 -days 1024 -out ssl/ca.crt -config ssl/ca_config.conf
```

### Create Server Certificates
```
openssl genrsa -out ssl/server.key 2048
openssl req -new -key ssl/server.key -out ssl/server.csr -config ssl/ca_config.conf
openssl x509 -req -days 360 -in ssl/server.csr -CA ssl/ca.crt -CAkey ssl/ca.key -CAcreateserial -out ssl/server.crt
```

### Generate DH params file
```
openssl dhparam -out ssl/dh_params.dh 2048
```

## Running Redis with SSL 
To start Redis with ssl enabled, you need to provide the parameter enable-ssl=true, certificate-file, private-key-file, and dh-params-file. Instructions to create these files can be found in "Creating SSL files needed for testing or production use." You also need to specify --ssl-port and --unixsocket. 