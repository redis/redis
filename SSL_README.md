# Redis with SSL quickstart guide:

## Overview of Redis with SSL
The SSL functionality enables encryption in-transit for all communication between clients and Redis server, between master and replicas and also communication between all nodes for Redis cluster mode. 

The SSL implementation uses S2N for the high level APIs and uses the libcrypto from OpenSSL v1.0.x for low level cryptographic implementations. All of the implementation for SSL functions can be found in ssl.c and ssl.h.

This feature is still considered experimental.

## Compatibility with Redis built without SSL
Redis with SSL is generally not compatible with Redis versions that are built without SSL. Although some limited circumstances may work, it is not recommended to run Redis built with SSL in the same cluster as Redis built without SSL.

### Cluster bus changes
The cluster bus protocol contains an extra attribute field for Redis with SSL, which means that nodes built with SSL will be unable to meet with Redis nodes built without SSL. This change is used to propagate the cluster-announce-endpoint to other clusters, so they can respond with the correct endpoint for clients.

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

### Troubleshooting building openssl and s2n on Ubuntu
The following packages are required to build ssl with ubuntu
```
sudo apt-get install xutils-dev libssl-dev tcltls
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
../src/redis-server --loglevel debug  --enable-ssl yes --certificate-file ../ssl/server.crt --private-key-file ../ssl/server.key --root-ca-certs-path ../ssl/ca.crt --dh-params-file ../ssl/dh_params.dh --port 6401 --cluster-announce-endpoint c1.cache.com no --cluster-enabled yes --cluster-interface-type dns > nodes1.log &
cd ..

cd node2
../src/redis-server --loglevel debug  --enable-ssl yes --certificate-file ../ssl/server.crt --private-key-file ../ssl/server.key --root-ca-certs-path ../ssl/ca.crt --dh-params-file ../ssl/dh_params.dh  --port 6402  --cluster-announce-endpoint c2.cache.com no --cluster-enabled yes --cluster-interface-type dns > nodes2.log &
cd ..

./src/redis-cli -h 127.0.0.1 -p 6401 --ssl cluster meet 127.0.0.1 6402
./src/redis-cli -h 127.0.0.1 -p 6401 --ssl cluster nodes
./src/redis-cli -h 127.0.0.1 -p 6401 --ssl info
```

### Client support
Most popular Redis libraries support SSL with a flag. 

### Redis-cli and Redis-benchmark
Redis-cli and Redis-benchmark both support ssl by providing the --ssl flag, you can see an example in the above section about launching a 2 node cluster with SSL enabled.

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

### Cluster announce endpoint
In order to use hostname validation as part of the SSL handshake, Redis with SSL supports announcing its endpoint using the cluster-announce-endpoint and cluster-interface-type parameters. Setting the cluster-interface-type to DNS will make cluster re-direct calls and cluster slot calls provide the cluster announce endpoint instead of the IP address. 

## Developing features that are compatible with SSL
Developing a feature for support with SSL should be hidden behind "#ifdef BUILD_SSL" if it needs to access s2n or OpenSSL libraries. This is to maintain a smaller executable size and limit performance impact for Redis built without SSL. There are several helper functions defined to make the Redis code cleaner, and new ones should be defined if there are frequent uses of "#ifdef BUILD_SSL".

### Initializing SSL connections
All SSL connections need to be initialized after the underlying TCP connection has been established and then they need to perform an SSL handshake. SSL negotiations are not handled synchronously in most places in the codebase - this is to prevent blocking the server while the SSL handshake is waiting for the the client/server to respond. Instead, you should use the sslNegotiate methods provided in ssl.c to perform asynchronous negotiation. Calling these functions will add a fileEvent to the event loop to handle the negotiation and your callback function will be called when the SSL handshake is completed. If the handshake fails, the client will be disconnected.

### Defined helper functions for SSL support
There are four defined helper functions for supporting SSL. These are used to minimize the amount of #ifdef's used in order to improve readability and maintainability of the code. These helper functions are defined in server.h:
1. rwrite(fd, buffer, bytes): Write data to a fd that would be associated with an ssl connection when ssl is enabled.
2. rread(fd, buffer, bytes): Read data from a fd that would be associated with an ssl connection when ssl is enabled.
3. rping(fd): Send a ping to a fd that would be associated with an ssl connection when ssl is enabled.
4. rclose(fd): Closes a fd that would be associated with an ssl connection when ssl is enabled.

### Concerns with separate processes
If an SSL connection is created in the main process, after a fork both the child and parent processes can’t continue to use the connection. An example of this can be found in socket based BGSAVE, where the master forks the main process to write the data to the client. The SSL connection has to be re-negotiated once the transfer of data is completed. There is an exception to this, if the child and parent are using the SSL connection for different operations, i.e. the parent can use the connection to just write data, and the child can use the connection to just read data.
