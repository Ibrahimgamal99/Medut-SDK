#!/bin/sh
# TLS and WSS take one PEM with certificate and key; build it from the
# shared interop certificate (test/interop/gen-certs.sh), then hand over to
# the image's own entrypoint.  /etc/freeswitch is already populated, so that
# entrypoint keeps our configuration.
set -e
mkdir -p /etc/freeswitch/tls
if [ -f /certs/interop.crt ]; then
	cat /certs/interop.crt /certs/interop.key > /etc/freeswitch/tls/wss.pem
	cp /etc/freeswitch/tls/wss.pem /etc/freeswitch/tls/agent.pem
	cp /certs/interop.crt /etc/freeswitch/tls/cafile.pem
fi
exec /entrypoint.sh
