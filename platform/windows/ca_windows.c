/**
 * @file ca_windows.c  Windows CA trust store
 *
 * libre never calls SSL_CTX_set_default_verify_paths(), and on Windows there
 * is nothing useful for it to find anyway: the trust store lives in the
 * registry-backed system certificate store, not as a PEM on disk.  So the
 * X509_STORE behind uag_tls() starts empty and every TLS/WSS registration
 * fails the handshake with "unable to get local issuer certificate".  See
 * platform/ios/ca_ios.c for the same problem on iOS.
 *
 * Export the "ROOT" system store — which includes anything a domain policy
 * has pushed — into one PEM for OpenSSL to read as a CAfile.  Rebuilt on each
 * init so trust changes are picked up.
 *
 * Note this exports what the store holds now; Windows also fetches roots on
 * demand from Windows Update, and a root that has never been needed on this
 * machine will not be present.  A deployment relying on one has to pass its
 * own bundle as medut_config_t.ca_cert_path.
 */

#include <stdio.h>
/* winsock2.h before windows.h: windows.h otherwise pulls the Winsock 1.1
 * header, which conflicts with the winsock2 definitions re.h needs. */
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <re.h>
#include "../../src/medut_internal.h"

const char *bsdk_platform_ca_bundle(const char *dir)
{
	static char path[700];
	HCERTSTORE store;
	PCCERT_CONTEXT ctx = NULL;
	unsigned written = 0;
	FILE *out;

	if (!dir || !*dir)
		return NULL;

	store = CertOpenSystemStoreA(0, "ROOT");
	if (!store) {
		warning("Medut: cannot open the Windows ROOT certificate "
		        "store; TLS server verification will fail unless "
		        "ca_cert_path is set\n");
		return NULL;
	}

	(void)re_snprintf(path, sizeof(path), "%s\\windows-ca-bundle.pem", dir);

	out = fopen(path, "w");
	if (!out) {
		CertCloseStore(store, 0);
		return NULL;
	}

	while ((ctx = CertEnumCertificatesInStore(store, ctx)) != NULL) {
		DWORD len = 0;
		char *pem;

		if (ctx->dwCertEncodingType != X509_ASN_ENCODING)
			continue;

		/* Ask for the size first — CryptBinaryToStringA writes the
		 * BEGIN/END armour and line breaks for us. */
		if (!CryptBinaryToStringA(ctx->pbCertEncoded,
		                          ctx->cbCertEncoded,
		                          CRYPT_STRING_BASE64HEADER,
		                          NULL, &len) || !len)
			continue;

		pem = mem_zalloc(len + 1, NULL);
		if (!pem)
			continue;

		if (CryptBinaryToStringA(ctx->pbCertEncoded,
		                         ctx->cbCertEncoded,
		                         CRYPT_STRING_BASE64HEADER,
		                         pem, &len)) {
			if (fputs(pem, out) >= 0)
				++written;
		}

		mem_deref(pem);
	}

	fclose(out);
	CertCloseStore(store, 0);

	if (!written) {
		(void)remove(path);
		warning("Medut: the Windows ROOT store held no usable "
		        "certificates; TLS server verification will fail "
		        "unless ca_cert_path is set\n");
		return NULL;
	}

	return path;
}
