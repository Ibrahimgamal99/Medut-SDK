CC      ?= gcc
CFLAGS  := -Wall -Wextra -std=c11 -O2 -g
LDFLAGS := -lm

TESTS := test_mos test_url_parser test_mwi_parser test_codec_str test_transport_str

# test_ws_pin links the real src/ws_path.c, so it needs the re/baresip headers
# and libs a host build installs. Built only when that sysroot is present
# (SYSROOT=... to point elsewhere); the other tests stay standalone.
SYSROOT ?= ../../build/linux-x86_64/sysroot
ifneq ($(wildcard $(SYSROOT)/include/re/re.h),)
TESTS += test_ws_pin test_audio_external test_fmtp_bitrate test_ice_deadline \
         test_dispatch_starve test_ice_candpair
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

# Links libre but not libbaresip: baresip_mnatl() needs a full baresip_init(),
# and the test only wants a media-NAT registry entry to interpose on, so it
# supplies its own along with the three other baresip symbols ice_shim.c uses.
test_ice_deadline: test_ice_deadline.c ../../src/ice_shim.c
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

# Exercises the two ice/candpair.c hunks in cmake/patch-re-sources.cmake via
# libre's internal ice.h.  Fails against a libre built from an unpatched tree
# — rebuild the sysroot after a libre bump before trusting a red run here.
test_ice_candpair: test_ice_candpair.c
	$(CC) $(CFLAGS) -Wno-unused-parameter \
	  -I$(SYSROOT)/include -I$(SYSROOT)/include/re -I../../include \
	  -o $@ $^ -L$(SYSROOT)/lib -lre -lssl -lcrypto -lpthread -lz \
	  $(LDFLAGS)

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
	rm -f $(TESTS)
