# FreeSWITCH 1.11 as a Medut interop target: the stock vanilla
# configuration, made deterministic and LAN-friendly.
#
#   - password "medut" for users 1000-1019 (vanilla picks a random one)
#   - TLS on 5061; WS 5066 and WSS 7443 as vanilla has them
#   - its own address advertised, instead of a public-STUN lookup at boot
#   - private ICE candidates accepted: by default FreeSWITCH drops them
#     (apply-candidate-acl), and everything on this test bridge is private —
#     the same rule that answers 488 to a host-only offer in production
#
# Dial 9196 for an echo, 9197 for a 1004 Hz tone, or another user (1000-1019).
FROM safarov/freeswitch:latest
RUN mkdir -p /etc/freeswitch \
 && cp -a /usr/share/freeswitch/conf/vanilla/. /etc/freeswitch/ \
 && sed -i \
      -e 's/default_password=1234/default_password=medut/' \
      -e 's/internal_ssl_enable=false/internal_ssl_enable=true/' \
      -e 's/external_rtp_ip=stun:stun.freeswitch.org/external_rtp_ip=auto/' \
      -e 's/external_sip_ip=stun:stun.freeswitch.org/external_sip_ip=auto/' \
      /etc/freeswitch/vars.xml \
 && sed -i \
      -e 's|<param name="ext-rtp-ip" value="$${external_rtp_ip}"/>|<param name="ext-rtp-ip" value="$${local_ip_v4}"/>|' \
      -e 's|<param name="ext-sip-ip" value="$${external_sip_ip}"/>|<param name="ext-sip-ip" value="$${local_ip_v4}"/>|' \
      -e 's|<param name="ws-binding"  value=":5066"/>|<param name="apply-candidate-acl" value="rfc1918.auto"/>\n    <param name="apply-candidate-acl" value="localnet.auto"/>\n    &|' \
      /etc/freeswitch/sip_profiles/internal.xml \
 && grep -q 'default_password=medut' /etc/freeswitch/vars.xml \
 && grep -q 'apply-candidate-acl' /etc/freeswitch/sip_profiles/internal.xml \
 && grep -q 'ext-rtp-ip" value="$${local_ip_v4}' /etc/freeswitch/sip_profiles/internal.xml
COPY start-interop.sh /start-interop.sh
ENTRYPOINT ["/bin/sh", "/start-interop.sh"]
