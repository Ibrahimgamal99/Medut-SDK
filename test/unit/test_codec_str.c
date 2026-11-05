/**
 * @file test_codec_str.c  Unit tests for codec list string builder
 *
 * Tests the codec_list_str function that converts codec enum arrays
 * to codec strings the stack's lookup takes like "opus/48000/2,PCMU/8000/1".
 */

#include <stdio.h>
#include <string.h>

#define TEST_PASS 0
#define TEST_FAIL 1

static int g_pass = 0;
static int g_fail = 0;

typedef enum {
	CODEC_OPUS = 0,
	CODEC_PCMU,
	CODEC_PCMA,
	CODEC_G722,
} codec_t;

static void codec_list_str(const codec_t *codecs, int count,
                              char *buf, size_t sz)
{
	buf[0] = '\0';
	for (int i = 0; i < count && i < 8; i++) {
		const char *name;
		switch (codecs[i]) {
		case CODEC_OPUS: name = "opus/48000/2";  break;
		case CODEC_PCMU: name = "PCMU/8000/1";   break;
		case CODEC_PCMA: name = "PCMA/8000/1";   break;
		case CODEC_G722: name = "G722/8000/1";   break;
		default:         name = NULL;             break;
		}
		if (!name) continue;
		if (buf[0]) strncat(buf, ",", sz - strlen(buf) - 1);
		strncat(buf, name, sz - strlen(buf) - 1);
	}
}

#define ASSERT_EQ_STR(name, a, b) do { \
	if (strcmp((a), (b)) != 0) { \
		fprintf(stderr, "FAIL: %s = \"%s\" (expected \"%s\")\n", name, (a), (b)); \
		g_fail++; \
	} else { g_pass++; } \
} while (0)

static void test_single_opus(void)
{
	char buf[256];
	codec_t codecs[] = { CODEC_OPUS };
	codec_list_str(codecs, 1, buf, sizeof(buf));
	ASSERT_EQ_STR("single_opus", buf, "opus/48000/2");
}

static void test_multiple_codecs(void)
{
	char buf[256];
	codec_t codecs[] = { CODEC_OPUS, CODEC_PCMU, CODEC_PCMA };
	codec_list_str(codecs, 3, buf, sizeof(buf));
	ASSERT_EQ_STR("multi_codec", buf, "opus/48000/2,PCMU/8000/1,PCMA/8000/1");
}

static void test_all_codecs(void)
{
	char buf[256];
	codec_t codecs[] = { CODEC_OPUS, CODEC_PCMU, CODEC_PCMA, CODEC_G722 };
	codec_list_str(codecs, 4, buf, sizeof(buf));
	ASSERT_EQ_STR("all_codecs", buf, "opus/48000/2,PCMU/8000/1,PCMA/8000/1,G722/8000/1");
}

static void test_empty(void)
{
	char buf[256];
	codec_t codecs[] = { CODEC_OPUS };
	codec_list_str(codecs, 0, buf, sizeof(buf));
	ASSERT_EQ_STR("empty", buf, "");
}

static void test_g722_only(void)
{
	char buf[256];
	codec_t codecs[] = { CODEC_G722 };
	codec_list_str(codecs, 1, buf, sizeof(buf));
	ASSERT_EQ_STR("g722_only", buf, "G722/8000/1");
}

static void test_pcma_pcmu_order(void)
{
	char buf[256];
	codec_t codecs[] = { CODEC_PCMA, CODEC_PCMU };
	codec_list_str(codecs, 2, buf, sizeof(buf));
	ASSERT_EQ_STR("pcma_pcmu", buf, "PCMA/8000/1,PCMU/8000/1");
}

int main(void)
{
	test_single_opus();
	test_multiple_codecs();
	test_all_codecs();
	test_empty();
	test_g722_only();
	test_pcma_pcmu_order();

	printf("Codec string tests: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail > 0 ? TEST_FAIL : TEST_PASS;
}
