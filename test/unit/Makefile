CC      ?= gcc
CFLAGS  := -Wall -Wextra -std=c11 -O2 -g
LDFLAGS := -lm

TESTS := test_mos test_url_parser test_mwi_parser test_codec_str test_transport_str \
         test_sip_status test_stats_loss

# test_ws_pin links the real src/api/ws_path.c, so it needs the libre headers
# and libs a host build installs. Built only when that sysroot is present
# (SYSROOT=... to point elsewhere); the other tests stay standalone.
SYSROOT ?= ../../build/linux-x86_64/sysroot
ifneq ($(wildcard $(SYSROOT)/include/re/re.h),)
TESTS += test_ws_pin test_audio_external test_fmtp_bitrate \
         test_dispatch_starve test_ice_sdp test_ice_pair test_ice_agent \
         test_ice_mod test_ice_turn_stream test_audio_processing
# The WebRTC run needs the library's headers to build; it SKIPs at runtime
# when the library itself cannot be loaded.
ifeq ($(shell pkg-config --exists webrtc-audio-processing-1 && echo y),y)
TESTS += test_audio_processing_webrtc
endif
# test_aurecv_plc and test_jbuf_adapt pin the receive-side concealment and
# the adaptive jitter buffer: they compile src/media/aureceiver.c and
# src/media/jbuf.c themselves, against libre only.
TESTS += test_aurecv_plc test_jbuf_adapt
# test_dns_parallel pins patch W of cmake/patch-re-sources.cmake (every
# nameserver asked at once) against libre's DNS client, with fake servers.
TESTS += test_dns_parallel
# test_sipreg_takeover pins patch Y (a replacing registration client removes
# the old binding in its own REGISTER) against libre, with a fake registrar.
TESTS += test_sipreg_takeover
endif

.PHONY: all clean test

all: $(TESTS)

test_mos: test_mos.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_url_parser: test_url_parser.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_mwi_parser: test_mwi_parser.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_codec_str: test_codec_str.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_transport_str: test_transport_str.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Includes src/api/sip_status.h directly — the header is free of libre so the
# shipped mapping is the one under test, with no sysroot needed.
test_sip_status: test_sip_status.c ../../src/api/sip_status.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Includes src/api/stats_loss.h directly — free of libre, like sip_status.h.
test_stats_loss: test_stats_loss.c ../../src/api/stats_loss.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# No --wrap: ws_path.c owns the public websock_connect/sip_dialog_route names
# outright (libre's definitions are renamed to __real_* by
# cmake/patch-re-sources.cmake), so this test links the exact symbols the
# shipped library does; the test provides the __real_* stand-ins.
test_ws_pin: test_ws_pin.c ../../src/api/ws_path.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib \
	  -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

test_dns_parallel: test_dns_parallel.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

test_sipreg_takeover: test_sipreg_takeover.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# Links libre alone: the test provides the device registry itself.
test_audio_external: test_audio_external.c ../../src/audio/external.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# Links libre only: the test stubs the handful of stack symbols (core/mdt.h)
# src/api/adapt.c references.
test_fmtp_bitrate: test_fmtp_bitrate.c ../../src/api/adapt.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# Links src/api/dispatch.c and src/api/re_loop.c against libre only (they read
# g_bsdk, which the test defines, and nothing else from the SDK).  Holds all
# of libre's async workers and times a dispatch: with the mqueue-based
# dispatch.c it lands in milliseconds, with re_thread_async_main() it lands
# when the first worker frees up.
test_dispatch_starve: test_dispatch_starve.c ../../src/api/dispatch.c ../../src/api/re_loop.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# medut-ice engine files link against libre alone: they include no header of
# the rest of the stack by design (see src/ice/ice_internal.h), so the
# agent's candidate, pairing and check logic is testable on its own.
ICE_ENGINE_SRCS := ../../src/ice/ice_gen.c ../../src/ice/ice_port.c \
                   ../../src/ice/ice_cand.c ../../src/ice/ice_sdp.c \
                   ../../src/ice/ice_pair.c ../../src/ice/ice_check.c \
                   ../../src/ice/ice_stund.c ../../src/ice/ice_select.c \
                   ../../src/ice/ice_route.c ../../src/ice/ice_gather.c

test_ice_sdp: test_ice_sdp.c $(ICE_ENGINE_SRCS)
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

test_ice_pair: test_ice_pair.c $(ICE_ENGINE_SRCS)
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

test_ice_turn_stream: test_ice_turn_stream.c $(ICE_ENGINE_SRCS)
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

test_ice_agent: test_ice_agent.c $(ICE_ENGINE_SRCS)
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# The module itself, with the stack's call/stream/log symbols stubbed — the
# same discipline as test_ice_deadline (no link against the stack).
# glibc 2.42+ declares once_flag in <stdlib.h> under _GNU_SOURCE, and
# re_thread.h typedefs its own unless ONCE_FLAG_INIT is already defined:
# pre-including <stdlib.h> lets re see glibc's and skip the duplicate.
test_ice_mod: test_ice_mod.c $(ICE_ENGINE_SRCS) ../../src/ice/ice_mod.c
	$(CC) $(CFLAGS) -Wno-unused-parameter -D_GNU_SOURCE -include stdlib.h \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include -I../../src \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# Links src/audio/processing.c against libre only; the stack's filter registry
# and the platform hooks are stubbed in the test, and tmr_jiffies is wrapped
# so the test owns the clock.
AP_FLAGS := -Wno-unused-parameter -D_GNU_SOURCE \
  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include -I../../src
AP_LIBS  := -Wl,--wrap=tmr_jiffies -L$(SYSROOT)/lib -lre -lssl -lcrypto \
  -lpthread -lz $(LDFLAGS)

test_audio_processing: test_audio_processing.c ../../src/audio/processing.c
	$(CC) $(CFLAGS) $(AP_FLAGS) -o $@ $^ $(AP_LIBS)

# Same test with the WebRTC filter compiled in, through the real shim: the
# wrapper needs no libstdc++, so it links as C, and loads the library by the
# SONAME the headers belong to — exactly what the SDK does.
CXX ?= g++
WEBRTC_CFLAGS := $(shell pkg-config --cflags webrtc-audio-processing-1 2>/dev/null)
WEBRTC_LIBDIR := $(shell pkg-config --variable=libdir webrtc-audio-processing-1 2>/dev/null)
WEBRTC_SONAME := $(shell objdump -p $(WEBRTC_LIBDIR)/libwebrtc-audio-processing-1.so 2>/dev/null | awk '/SONAME/{print $$2}')

webrtc_apm_test.o: ../../src/audio/webrtc_apm.cpp ../../src/audio/webrtc_apm.h
	$(CXX) -std=c++17 -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	  $(subst -I,-isystem ,$(WEBRTC_CFLAGS)) -c -o $@ $<

test_audio_processing_webrtc: test_audio_processing.c ../../src/audio/processing.c \
                              ../../src/audio/webrtc_shim.c webrtc_apm_test.o
	$(CC) $(CFLAGS) $(AP_FLAGS) -DBSDK_TEST_WEBRTC \
	  -DMEDUT_PROFILE_DESKTOP=1 -DMEDUT_HAS_WEBRTC_AEC=1 \
	  -DMEDUT_WEBRTC_SONAME=\"$(WEBRTC_SONAME)\" \
	  -o $@ $^ $(AP_LIBS) -ldl

MEDIA_CFLAGS = -Wno-unused-parameter -D_GNU_SOURCE -I../../src -I../../include \
	-I$(SYSROOT)/include -I$(SYSROOT)/include/re

test_aurecv_plc: test_aurecv_plc.c ../../src/media/aureceiver.c \
                 ../../src/media/timestamp.c
	$(CC) $(CFLAGS) $(MEDIA_CFLAGS) -o $@ $^ \
	  -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz $(LDFLAGS)

# On a simulated clock: the test drives jbuf.c the way rtprecv.c does.
test_jbuf_adapt: test_jbuf_adapt.c ../../src/media/jbuf.c
	$(CC) $(CFLAGS) $(MEDIA_CFLAGS) -o $@ $^ -Wl,--wrap=tmr_jiffies \
	  -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz $(LDFLAGS)

test: all
	@failed=0; \
	for t in $(TESTS); do \
		echo "Running $$t..."; \
		./$$t || failed=$$((failed + 1)); \
	done; \
	if [ $$failed -eq 0 ]; then \
		echo "All unit tests passed."; \
	else \
		echo "$$failed test(s) failed."; \
		exit 1; \
	fi

clean:
	rm -f $(TESTS) test_aurecv_plc test_jbuf_adapt webrtc_apm_test.o
