CC      ?= gcc
CFLAGS  := -Wall -Wextra -std=c11 -O2 -g
LDFLAGS := -lm

TESTS := test_mos test_url_parser test_mwi_parser test_codec_str test_transport_str \
         test_sip_status test_stats_loss

# test_ws_pin links the real src/ws_path.c, so it needs the re/baresip headers
# and libs a host build installs. Built only when that sysroot is present
# (SYSROOT=... to point elsewhere); the other tests stay standalone.
SYSROOT ?= ../../build/linux-x86_64/sysroot
ifneq ($(wildcard $(SYSROOT)/include/re/re.h),)
TESTS += test_ws_pin test_audio_external test_fmtp_bitrate \
         test_dispatch_starve test_ice_sdp test_ice_pair test_ice_agent \
         test_ice_mod test_audio_processing
# The WebRTC run needs the library's headers to build; it SKIPs at runtime
# when the library itself cannot be loaded.
ifeq ($(shell pkg-config --exists webrtc-audio-processing-1 && echo y),y)
TESTS += test_audio_processing_webrtc
endif
# Needs the baresip tree a build has patched, at the patch version the test
# was written for (cmake/patch-baresip-sources.cmake, job 6).
BARESIP_SRC ?= ../../third_party/baresip
ifneq ($(shell grep -l "echosdk-patch aurecv-plc v2" $(BARESIP_SRC)/src/aureceiver.c 2>/dev/null),)
TESTS += test_aurecv_plc
endif
# Same for the jitter buffer's adaptive sizing (job 8).
ifneq ($(shell grep -l "echosdk-patch jbuf-adapt v1" $(BARESIP_SRC)/src/jbuf.c 2>/dev/null),)
TESTS += test_jbuf_adapt
endif
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

# Includes src/sip_status.h directly — the header is free of re/baresip so the
# shipped mapping is the one under test, with no sysroot needed.
test_sip_status: test_sip_status.c ../../src/sip_status.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Includes src/stats_loss.h directly — free of re/baresip, like sip_status.h.
test_stats_loss: test_stats_loss.c ../../src/stats_loss.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# No --wrap: ws_path.c owns the public websock_connect/sip_dialog_route names
# outright (libre's definitions are renamed to __real_* by
# cmake/patch-re-sources.cmake), so this test links the exact symbols the
# shipped library does; the test provides the __real_* stand-ins.
test_ws_pin: test_ws_pin.c ../../src/ws_path.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib \
	  -lbaresip -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# Deliberately does NOT link libbaresip: baresip_ausrcl() sits in the same
# object as the player and recorder, so pulling it in drags every audio backend
# the sysroot happens to have been built with. The test provides the device
# registry itself and links libre alone.
test_audio_external: test_audio_external.c ../../src/audio_external.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# Links libre but not libbaresip, for the reason given on test_audio_external:
# the test stubs the handful of baresip symbols src/adapt.c references.
test_fmtp_bitrate: test_fmtp_bitrate.c ../../src/adapt.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# Links src/dispatch.c and src/re_loop.c against libre only (they read
# g_bsdk, which the test defines, and nothing else from the SDK).  Holds all
# of libre's async workers and times a dispatch: with the mqueue-based
# dispatch.c it lands in milliseconds, with re_thread_async_main() it lands
# when the first worker frees up.
test_dispatch_starve: test_dispatch_starve.c ../../src/dispatch.c ../../src/re_loop.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# echo-ice engine files link against libre alone: they include no baresip
# header by design (see src/ice/ice_internal.h), so the agent's candidate,
# pairing and check logic is testable without a baresip_init().
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

test_ice_agent: test_ice_agent.c $(ICE_ENGINE_SRCS)
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# The module itself, with baresip's call/stream/log/config symbols stubbed
# — the same discipline as test_ice_deadline (no libbaresip link).
test_ice_mod: test_ice_mod.c $(ICE_ENGINE_SRCS) ../../src/ice/ice_mod.c
	$(CC) $(CFLAGS) -Wno-unused-parameter -D_GNU_SOURCE \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include -I../../src \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# Links src/audio_processing.c against libre only; baresip's filter registry
# and the platform hooks are stubbed in the test, and tmr_jiffies is wrapped
# so the test owns the clock.
AP_FLAGS := -Wno-unused-parameter -D_GNU_SOURCE \
  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include -I../../src
AP_LIBS  := -Wl,--wrap=tmr_jiffies -L$(SYSROOT)/lib -lre -lssl -lcrypto \
  -lpthread -lz $(LDFLAGS)

test_audio_processing: test_audio_processing.c ../../src/audio_processing.c
	$(CC) $(CFLAGS) $(AP_FLAGS) -o $@ $^ $(AP_LIBS)

# Same test with the WebRTC filter compiled in, through the real shim: the
# wrapper needs no libstdc++, so it links as C, and loads the library by the
# SONAME the headers belong to — exactly what the SDK does.
CXX ?= g++
WEBRTC_CFLAGS := $(shell pkg-config --cflags webrtc-audio-processing-1 2>/dev/null)
WEBRTC_LIBDIR := $(shell pkg-config --variable=libdir webrtc-audio-processing-1 2>/dev/null)
WEBRTC_SONAME := $(shell objdump -p $(WEBRTC_LIBDIR)/libwebrtc-audio-processing-1.so 2>/dev/null | awk '/SONAME/{print $$2}')

webrtc_apm_test.o: ../../src/webrtc_apm.cpp ../../src/webrtc_apm.h
	$(CXX) -std=c++17 -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	  $(subst -I,-isystem ,$(WEBRTC_CFLAGS)) -c -o $@ $<

test_audio_processing_webrtc: test_audio_processing.c ../../src/audio_processing.c \
                              ../../src/webrtc_shim.c webrtc_apm_test.o
	$(CC) $(CFLAGS) $(AP_FLAGS) -DBSDK_TEST_WEBRTC \
	  -DECHOSDK_PROFILE_DESKTOP=1 -DECHOSDK_HAS_WEBRTC_AEC=1 \
	  -DECHOSDK_WEBRTC_SONAME=\"$(WEBRTC_SONAME)\" \
	  -o $@ $^ $(AP_LIBS) -ldl

# Compiles the patched aureceiver.c itself, against libre only: the test
# stubs the player registry and logging, and models the jitter buffer.
test_aurecv_plc: test_aurecv_plc.c $(BARESIP_SRC)/src/aureceiver.c \
                 $(BARESIP_SRC)/src/timestamp.c
	$(CC) $(CFLAGS) -Wno-unused-parameter -D_GNU_SOURCE \
	  -I$(BARESIP_SRC)/include -I$(BARESIP_SRC)/src \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

# Compiles the patched jbuf.c itself, against libre only, on a simulated
# clock: the test drives it the way rtprecv.c does.
test_jbuf_adapt: test_jbuf_adapt.c $(BARESIP_SRC)/src/jbuf.c
	$(CC) $(CFLAGS) -Wno-unused-parameter -D_GNU_SOURCE \
	  -I$(BARESIP_SRC)/include -I$(BARESIP_SRC)/src \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re \
	  -o $@ $^ -Wl,--wrap=tmr_jiffies \
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
