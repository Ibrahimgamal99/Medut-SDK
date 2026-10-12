/**
 * @file ca_macos.c  macOS CA trust store
 *
 * We build against an OpenSSL whose X509_STORE starts empty — libre never
 * calls SSL_CTX_set_default_verify_paths() — so without a CAfile every
 * TLS/WSS registration fails the handshake with "unable to get local issuer
 * certificate".  See platform/ios/ca_ios.c for the same problem on iOS.
 *
 * macOS, unlike iOS, does expose its anchors: SecTrustCopyAnchorCertificates()
 * returns the system roots *plus* anything trusted in the admin and user
 * keychains, so an enterprise or MDM root works without configuration.  We
 * re-export them as one PEM on each init, which also picks up trust changes
 * made since the last launch.
 *
 * Falls back to Apple's own /etc/ssl/cert.pem (system roots only) if the
 * keychain query fails.
 */

#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <Security/Security.h>
#include <re.h>
#include "../../src/medut_internal.h"

/* Longest anchor DER is ~2KB; 8KB leaves room and keeps this off the heap. */
#define BSDK_MAX_DER 8192

static bool bsdk_write_pem(FILE *out, const uint8_t *der, size_t len)
{
	/* 4 base64 chars per 3 bytes, plus a newline every 64 chars. */
	char b64[BSDK_MAX_DER * 4 / 3 + 64];
	size_t olen = sizeof(b64);
	size_t i;

	if (len > BSDK_MAX_DER)
		return false;
	if (base64_encode(der, len, b64, &olen))
		return false;

	if (fputs("-----BEGIN CERTIFICATE-----\n", out) < 0)
		return false;
	for (i = 0; i < olen; i += 64) {
		size_t n = (olen - i) < 64 ? (olen - i) : 64;
		if (fwrite(b64 + i, 1, n, out) != n || fputc('\n', out) < 0)
			return false;
	}
	return fputs("-----END CERTIFICATE-----\n", out) >= 0;
}

const char *bsdk_platform_ca_bundle(const char *dir)
{
	static const char *sysfile = "/etc/ssl/cert.pem";
	static char path[700];
	CFArrayRef anchors = NULL;
	CFIndex count, i;
	unsigned written = 0;
	FILE *out;

	if (!dir || !*dir)
		return NULL;

	if (SecTrustCopyAnchorCertificates(&anchors) != errSecSuccess || !anchors)
		goto fallback;

	(void)re_snprintf(path, sizeof(path), "%s/macos-ca-bundle.pem", dir);

	out = fopen(path, "w");
	if (!out) {
		CFRelease(anchors);
		goto fallback;
	}

	count = CFArrayGetCount(anchors);
	for (i = 0; i < count; i++) {
		SecCertificateRef cert =
			(SecCertificateRef)CFArrayGetValueAtIndex(anchors, i);
		CFDataRef der = cert ? SecCertificateCopyData(cert) : NULL;

		if (!der)
			continue;

		if (bsdk_write_pem(out, CFDataGetBytePtr(der),
		                   (size_t)CFDataGetLength(der)))
			++written;

		CFRelease(der);
	}

	fclose(out);
	CFRelease(anchors);

	if (written)
		return path;

	(void)remove(path);

fallback:
	if (!access(sysfile, R_OK))
		return sysfile;

	warning("Medut: no macOS trust anchors found; TLS server "
	        "verification will fail unless ca_cert_path is set\n");
	return NULL;
}
