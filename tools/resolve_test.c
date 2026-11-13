// PC test of src/resolve.c: runs the plugin's matcher on unpacked images.
//   resolve_test <xam.bin> <xam base hex> <dash.bin> <dash base hex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/resolve.h"

typedef struct {
	unsigned char *data;
	uint32_t base;
	uint32_t size;
} File;

static File xam;
static File dash;
static uint32_t export_table;

static File load(const char *path, const char *base)
{
	File f = { 0 };
	FILE *in = fopen(path, "rb");
	if (!in) {
		fprintf(stderr, "can't open %s\n", path);
		exit(1);
	}
	fseek(in, 0, SEEK_END);
	f.size = (uint32_t)ftell(in);
	fseek(in, 0, SEEK_SET);
	f.data = malloc(f.size);
	if (fread(f.data, 1, f.size, in) != f.size)
		exit(1);
	fclose(in);
	f.base = (uint32_t)strtoul(base, NULL, 16);
	return f;
}

static const File *owner(uint32_t address)
{
	if (address >= xam.base && address - xam.base <= xam.size - 4)
		return &xam;
	if (address >= dash.base && address - dash.base <= dash.size - 4)
		return &dash;
	return NULL;
}

uint32_t resolve_valid(uint32_t address)
{
	return owner(address) != NULL;
}

uint32_t resolve_read32(uint32_t address)
{
	const File *f = owner(address);
	if (!f) {
		fprintf(stderr, "read outside the images: %08X\n", address);
		exit(1);
	}
	const unsigned char *p = f->data + (address - f->base);
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// xam's XEX export table: magic, then count and ordinal base at words 8 and 9, then image-relative offsets.
uint32_t resolve_xam_export(uint32_t ordinal)
{
	const uint32_t count = resolve_read32(export_table + 32);
	const uint32_t first = resolve_read32(export_table + 36);
	if (ordinal < first || ordinal - first >= count)
		return 0;
	const uint32_t offset = resolve_read32(export_table + 40 + (ordinal - first) * 4);
	return offset ? xam.base + offset : 0;
}

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: resolve_test <xam.bin> <base> <dash.bin> <base>\n");
		return 1;
	}
	xam = load(argv[1], argv[2]);
	dash = load(argv[3], argv[4]);
	static const unsigned char magic[8] = { 0x00, 'H', 'V', 'E', 0x48, 0x00, 0x00, 0x00 };
	for (uint32_t i = 0; i + 8 < xam.size; i += 4) {
		if (!memcmp(xam.data + i, magic, 8)) {
			export_table = xam.base + i;
			break;
		}
	}
	if (!export_table) {
		fprintf(stderr, "no export table in xam\n");
		return 1;
	}
	const int found = resolve_all(dash.base);
	printf("found %d of %d, %d anchors\n", found, FN_COUNT, resolve_anchor_count());
	static const char *const how[] = { "NOT FOUND", "unique", "neighbour", "twin" };
	for (int i = 0; i < FN_COUNT; i++) {
		const ResolveFunction *f = &resolve_functions[i];
		printf("  %-30s xam %08X len %4X -> %08X  %s\n", f->name, f->xam, f->length, f->dash, how[f->how]);
	}
	// The saved-addresses path: the right addresses must pass, and one wrong address must fail the lot.
	uint32_t saved[FN_COUNT];
	for (int i = 0; i < FN_COUNT; i++)
		saved[i] = resolve_functions[i].dash;
	printf("  from saved addresses: %d of %d\n", resolve_from_saved(dash.base, saved), FN_COUNT);
	for (int i = 0; i < FN_COUNT; i++) {
		const uint32_t right = saved[i];
		saved[i] = right + 8;
		if (resolve_from_saved(dash.base, saved) == FN_COUNT)
			printf("  WRONG: a shifted %s was accepted\n", resolve_functions[i].name);
		saved[i] = right;
	}
	printf("  from saved addresses again: %d of %d\n", resolve_from_saved(dash.base, saved), FN_COUNT);
	const uint32_t wrapper = resolve_find_pattern(player_wrapper_words, player_wrapper_masks, PLAYER_WRAPPER_WORDS);
	printf("  player wrapper (16197: 927DB650) -> %08X\n", wrapper);
	return found == FN_COUNT ? 0 : 2;
}
