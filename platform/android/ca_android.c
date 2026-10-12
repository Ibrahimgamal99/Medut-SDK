/**
 * @file ca_android.c  Android CA trust store
 *
 * Moved here from core.c so every platform answers the same
 * bsdk_platform_ca_bundle() hook — see platform/ios/ca_ios.c for why the hook
 * exists at all (libre never calls SSL_CTX_set_default_verify_paths(), so an
 * unset CAfile means an empty X509_STORE and a failed handshake on every
 * TLS/WSS registration).
 */

#include <stdio.h>
#include <dirent.h>
#include <re.h>
#include "../../src/medut_internal.h"

/* ── Android CA bundle ───────────────────────────────────────────────────────
 *
 * Android ships its trust store as one PEM per CA, named by the certificate's
 * subject hash. Those names use OpenSSL's *old* MD5 hash; OpenSSL 1.0 switched
 * to a SHA-1 based name, so handing the directory to OpenSSL as a CApath finds
 * nothing and every server certificate fails with "unable to get local issuer
 * certificate". There is no CA bundle file on Android to point at instead.
 *
 * Concatenating the directory into one PEM sidesteps the naming entirely — a
 * CAfile is parsed start to end. Rebuilt on each init so OS trust-store
 * updates (and user-removed CAs) are picked up.
 *
 * Returns a static path on success, or NULL to leave ca_cert_path unset.
 */
const char *bsdk_platform_ca_bundle(const char *dir)
{
	/* Conscrypt's copy is the live store on Android 14+, where the platform
	 * one under /system can be stale. Prefer it, fall back for older. */
	static const char *srcdirs[] = {
		"/apex/com.android.conscrypt/cacerts",
		"/system/etc/security/cacerts",
	};
	static char path[700];
	size_t written = 0;

	(void)re_snprintf(path, sizeof(path), "%s/android-ca-bundle.pem", dir);

	FILE *out = fopen(path, "w");
	if (!out)
		return NULL;

	for (size_t i = 0; i < RE_ARRAY_SIZE(srcdirs) && !written; i++) {
		DIR *d = opendir(srcdirs[i]);
		if (!d)
			continue;

		struct dirent *ent;
		while ((ent = readdir(d)) != NULL) {
			char cert[768];
			char buf[4096];
			size_t n;

			if (ent->d_name[0] == '.')
				continue;

			(void)re_snprintf(cert, sizeof(cert), "%s/%s",
			                  srcdirs[i], ent->d_name);

			FILE *in = fopen(cert, "r");
			if (!in)
				continue;

			while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
				if (fwrite(buf, 1, n, out) != n)
					break;
				written += n;
			}
			fclose(in);
			/* Each file ends without a guaranteed newline. */
			fputc('\n', out);
		}
		closedir(d);
	}

	fclose(out);

	if (!written) {
		warning("Medut: no Android system CAs found; TLS server "
		        "verification will fail unless ca_cert_path is set\n");
		return NULL;
	}

	return path;
}
