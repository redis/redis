# Redis with SSL quickstart guide:

## Overview of Redis with SSL
The SSL functionality enables encryption in-transit for all communication between clients and Redis server, between master and replicas and also communication between all nodes for Redis cluster mode. 

The SSL implementation uses S2N for the high level APIs and uses the libcrypto from OpenSSL for low level cryptographic implementations. All of the implementation for SSL functions can be found in ssl.c and ssl.h.

This feature is still considered experimental.

## Compatibility with Redis built without SSL
Redis with SSL is generally not compatible with Redis versions that are built without SSL. Although some limited circumstances may work, it is not recommended to run Redis built with SSL in the same cluster as Redis built without SSL.

## Building SSL version of Redis
Redis with SSL requires building Redis with a new flag, "BUILD_SSL=yes". This builds a version of Redis that supports SSL. This build flag is used to limit the size of the Redis executable and optimize the code paths if SSL is not required. Redis built with SSL requires the installation of two dependencies, S2N and OpenSSL. These libraries are statically linked within redis, so they do not depend on their availability in whatever location you're installing your code. The assumption is that you will use the OpenSSL provided by your OS, but you can choose to build OpenSSL yourself and link it using the OPENSSL_LIB_DIR and OPENSSL_INCLUDE_DIR flags. 

### Download the latest version of s2n to use during the build
Redis does not yet come with a version of s2n, so it needs to be downloaded.

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

#### SSL libraries aren't available on my machine
Not all OS come with a version of ssl libraries. You can execute the following to install openssl on your machine from the root redis directory.

```
cd deps
rm -rf openssl
mkdir openssl
cd openssl
curl -LO https://www.openssl.org/source/openssl-1.0.2-latest.tar.gz
tar -xzvf openssl-1.0.2-latest.tar.gz
cp -r openssl-1.0.2*/* .
rm -f openssl-1.0.2-latest.tar.gz
echo "Downloaded latest version of openssl 1.0.2"
./config
make depend
make
cd ../..
```

You can then make redis with the following

```
OPENSSL_LIB_DIR=`pwd`/deps/openssl OPENSSL_INCLUDE_DIR=`pwd`/deps/openssl/include BUILD_SSL=yes make
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
To start Redis with ssl enabled, you need to provide the parameter enable-ssl=true, certificate-file, private-key-file, and dh-params-file. Instructions to create these files can be found in "Creating SSL files needed for testing or production use."

### Launch a 2 node Redis cluster with SSL enabled
```
rm -rf node1
rm -rf node2
mkdir node1
mkdir node2

cd node1
../src/redis-server --loglevel debug  --enable-ssl yes --certificate-file ../ssl/server.crt --private-key-file ../ssl/server.key --root-ca-certs-path ../ssl/ca.crt --dh-params-file ../ssl/dh_params.dh --port 6401 > nodes1.log &
cd ..

cd node2
../src/redis-server --loglevel debug  --enable-ssl yes --certificate-file ../ssl/server.crt --private-key-file ../ssl/server.key --root-ca-certs-path ../ssl/ca.crt --dh-params-file ../ssl/dh_params.dh  --port 6402 > nodes2.log &
cd ..

TODO: redis-cli doesn't support TLS, so there is no easy way to connect
```

### Client support
Most popular Redis libraries support SSL with a flag. 

## Configuration options for Redis with SSL
Redis with SSL adds several new configuration options.

### Runtime flag
Redis built with SSL has a start time flag controlling if SSL is enabled, "ssl-enabled". The values are yes or no.

### S2N performance mode
Redis with SSL provides a configuration that allows for tuning the performance settings. This can be set with ssl-performance-mode, which takes one of two values.
1. "low-latency", prefer low latency in ssl communication
2. "high-throughput", prefer high throughput in ssl communication

### Certificate renewals
Certificates can be renewed by calling "config set ssl-renew-certificate (path to new private key) (path to new certificate)." This will not terminate existing connections connected to the latest certificate. Redis will maintain the connections until either ssl-renew-certificate is called or the client disconnects and re-connects to the new certificate. 

## Developing features that are compatible with SSL
Developing a feature for support with SSL make use of the "isSSLCompiled()" or "isSSLEnabled()" macros. #ifdefs should be limited to ssl.c and ssl.h so as to not impair the readability of the main code base. 

### Initializing SSL connections
All SSL connections need to be initialized after the underlying TCP connection has been established and then they need to perform an SSL handshake. SSL negotiations are not handled synchronously in most places in the codebase - this is to prevent blocking the server while the SSL handshake is waiting for the the client/server to respond. Instead, you should use the sslNegotiate methods provided in ssl.c to perform asynchronous negotiation. Calling these functions will add a fileEvent to the event loop to handle the negotiation and your callback function will be called when the SSL handshake is completed. If the handshake fails, the client will be disconnected.

### System call wrapping
Macros are used to overwrite several system calls with wrappers, which determine whether or not they need to be handled as SSL fd. These are used to minimize the amount of #ifdef's used to improve readability and maintainability. These helper functions are defined in ssl.h and only applied within the redis codebase.

### Concerns with separate processes
If an SSL connection is created in the main process, after a fork both the child and parent processes can’t continue to use the connection. An example of this can be found in socket based BGSAVE, where the master forks the main process to write the data to the client. The SSL connection has to be re-negotiated once the transfer of data is completed. There is an exception to this, if the child and parent are using the SSL connection for different operations, i.e. the parent can use the connection to just write data, and the child can use the connection to just read data.
