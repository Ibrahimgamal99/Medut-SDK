/**
 * @file ca_linux.c  Linux CA trust store
 *
 * libre never calls SSL_CTX_set_default_verify_paths(), so OpenSSL's
 * compiled-in default CApath is never consulted and the X509_STORE behind
 * uag_tls() starts empty — every TLS/WSS registration then fails the
 * handshake with "unable to get local issuer certificate".  See
 * platform/ios/ca_ios.c for the same problem on iOS.
 *
 * Linux distributions do ship a concatenated bundle, they just disagree on
 * where.  Point OpenSSL at the first one that exists; the paths below cover
 * Debian/Ubuntu, Fedora/RHEL, SUSE, Alpine and Arch.
 */

#include <unistd.h>
#include <re.h>
#include "../../src/medut_internal.h"

const char *bsdk_platform_ca_bundle(const char *dir)
{
	static const char *bundles[] = {
		"/etc/ssl/certs/ca-certificates.crt",         /* Debian, Alpine */
		"/etc/pki/tls/certs/ca-bundle.crt",           /* Fedora, RHEL   */
		"/etc/ssl/ca-bundle.pem",                     /* SUSE           */
		"/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
		"/etc/ssl/cert.pem",                          /* Arch, misc     */
	};
	size_t i;

	(void)dir;  /* nothing to write — the distro bundle is used in place */

	for (i = 0; i < RE_ARRAY_SIZE(bundles); i++) {
		if (!access(bundles[i], R_OK))
			return bundles[i];
	}

	warning("Medut: no system CA bundle found; TLS server verification "
	        "will fail unless ca_cert_path is set\n");
	return NULL;
}
