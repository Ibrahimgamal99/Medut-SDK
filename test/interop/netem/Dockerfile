# Network impairment sidecar: shares the PBX's network namespace
# (network_mode: service:<pbx>) and shapes its traffic with tc netem, so the
# host needs no root and the impairment applies to everything the PBX sends.
FROM alpine:3.20
RUN apk add --no-cache iproute2-tc
CMD ["sleep", "infinity"]
