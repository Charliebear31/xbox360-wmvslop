// The dashboard (dash.xex) links its own copy of the XUI library; xam.xex exports the same library by ordinal.
// Both come from one system update, so a function in the dashboard has the same instructions as xam's export
// once position-dependent fields are ignored. That finds every function this plugin calls without an address
// table per dashboard version. Checked on the PC against dash 2.0.16197 and 2.0.14717 (tools/resolve_test.c).
#include "resolve.h"

ResolveFunction resolve_functions[FN_COUNT] = {
	[FN_CREATE_OBJECT] = { "XuiCreateObject", 0x325 },
	[FN_DESTROY_OBJECT] = { "XuiDestroyObject", 0x326 },
	[FN_ADD_CHILD] = { "XuiElementAddChild", 0x328 },
	[FN_INSERT_CHILD] = { "XuiElementInsertChild", 0x385 },
	[FN_UNLINK] = { "XuiElementUnlink", 0x384 },
	[FN_GET_PARENT] = { "XuiElementGetParent", 0x331 },
	[FN_GET_FIRST_CHILD] = { "XuiElementGetFirstChild", 0x32B },
	[FN_GET_NEXT] = { "XuiElementGetNext", 0x330 },
	[FN_GET_ID] = { "XuiElementGetId", 0x32E },
	[FN_GET_DESCENDANT_BY_ID] = { "XuiElementGetDescendantById", 0x83F },
	[FN_SET_BOUNDS] = { "XuiElementSetBounds", 0x336 },
	[FN_SET_OPACITY] = { "XuiElementSetOpacity", 0x37E },
	[FN_GET_OBJECT_CLASS] = { "XuiGetObjectClass", 0x33E },
	[FN_GET_PROPERTY_ID] = { "XuiObjectGetPropertyId", 0x348 },
	[FN_SET_PROPERTY] = { "XuiObjectSetProperty", 0x37C },
	[FN_GET_FOCUS] = { "XuiElementGetFocus", 0x32C },
	[FN_GET_USER_FOCUS] = { "XuiElementGetUserFocus", 0x332 },
	[FN_OBJECT_FROM_HANDLE] = { "XuiObjectFromHandle", 0x346 },
	[FN_GET_IMAGE_PATH] = { "XuiImageElementGetImagePath", 0x394 },
};

// From dash 2.0.16197, 927DB650 (decomp/player.c): clears a 72-byte parameter block, sets its type to 3
// (memory), stores the buffer and size, and calls the general player creator. Only the call's displacement
// depends on where the code sits.
const uint32_t player_wrapper_words[PLAYER_WRAPPER_WORDS] = {
	0x7D8802A6, 0x9181FFF8, 0x9421FF60, 0x39400009, 0x39610048, 0x7CA82B78, 0x39200000, 0x7D4903A6,
	0xF92B0009, 0x4200FFFC, 0x39600003, 0x90810070, 0x91010074, 0x7CC53378, 0x91610050, 0x38810050,
	0x4BFFF869, 0x382100A0, 0x8181FFF8, 0x7D8803A6, 0x4E800020,
};
const uint32_t player_wrapper_masks[PLAYER_WRAPPER_WORDS] = {
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	0xFC000003, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
};

enum { MAX_ANCHORS = 320, MAX_TWINS = 16, LEAF_MAX_WORDS = 40, LEAF_MIN_WORDS = 3 };

// xam exports whose code is found exactly once in the dashboard.
static struct {
	uint32_t xam;
	uint32_t dash;
} anchors[MAX_ANCHORS];
static int anchor_count;

static ResolveImage xam_image;
static ResolveImage dash_image;

int resolve_anchor_count(void)
{
	return anchor_count;
}

// PE header fields are little-endian; the console is big-endian.
static uint32_t read_le32(uint32_t address)
{
	const uint32_t v = resolve_read32(address);
	return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24;
}

int resolve_open_image(uint32_t base, ResolveImage *image)
{
	if (!resolve_valid(base) || resolve_read32(base) >> 16 != 0x4D5A)
		return -1;
	const uint32_t pe = base + read_le32(base + 0x3C);
	if (pe - base > 0x800 || !resolve_valid(pe) || resolve_read32(pe) != 0x50450000)
		return -1;
	const uint32_t section_count = read_le32(pe + 4) >> 16 & 0xFFFF;
	const uint32_t optional_bytes = read_le32(pe + 20) & 0xFFFF;
	image->base = base;
	image->size = read_le32(pe + 24 + 56);
	image->sections = pe + 24 + optional_bytes;
	image->section_count = section_count;
	image->pdata = 0;
	image->pdata_count = 0;
	for (uint32_t i = 0; i < section_count; i++) {
		const uint32_t section = image->sections + i * 40;
		// ".pdata\0\0"
		if (resolve_read32(section) == 0x2E706461 && resolve_read32(section + 4) == 0x74610000) {
			image->pdata_count = read_le32(section + 8) / 8;
			image->pdata = base + read_le32(section + 12);
		}
	}
	return image->pdata && image->pdata_count ? 0 : -1;
}

// .pdata: begin address, then a word whose bits 8..29 are the length in instructions.
static uint32_t pdata_begin(const ResolveImage *image, uint32_t index)
{
	return resolve_read32(image->pdata + index * 8);
}

static uint32_t pdata_length(const ResolveImage *image, uint32_t index)
{
	return (resolve_read32(image->pdata + index * 8 + 4) >> 8 & 0x3FFFFF) * 4;
}

// The table is sorted by address.
static uint32_t function_length(const ResolveImage *image, uint32_t address)
{
	uint32_t low = 0;
	uint32_t high = image->pdata_count;
	while (low < high) {
		const uint32_t middle = low + (high - low) / 2;
		const uint32_t begin = pdata_begin(image, middle);
		if (begin == address)
			return pdata_length(image, middle);
		if (begin != 0 && begin < address)
			low = middle + 1;
		else
			high = middle;
	}
	return 0;
}

static int in_image(const ResolveImage *image, uint32_t address, uint32_t bytes)
{
	return address >= image->base && address - image->base <= image->size - bytes && bytes <= image->size;
}

// Compares two runs of instructions, ignoring what depends on where the code sits: branch displacements, lis
// immediates, and the displacement of an instruction whose base register lis loaded. Kept identical to
// tools/xuimatch.py's norm(), which the offline survey used.
static int code_equal(uint32_t a, uint32_t b, uint32_t words)
{
	uint32_t lis = 0;  // bit per register holding an address half
	for (uint32_t i = 0; i < words; i++) {
		const uint32_t x = resolve_read32(a + i * 4);
		const uint32_t y = resolve_read32(b + i * 4);
		const uint32_t op = x >> 26;
		const uint32_t rd = x >> 21 & 31;
		const uint32_t ra = x >> 16 & 31;
		uint32_t mask = 0xFFFFFFFF;
		if (op == 18) {
			mask = 0xFC000003;
		} else if (op == 16) {
			mask = 0xFFFFFFFF;
		} else if (op == 15 && ra == 0) {
			lis |= 1u << rd;
			mask = 0xFFFF0000;
		} else if ((op == 14 || (op >= 32 && op <= 55) || op == 24 || op == 58 || op == 62) && (lis >> ra & 1)) {
			if (op == 14)
				lis |= 1u << rd;
			mask = 0xFFFF0000;
		} else if (op == 14 || op == 15 || op == 31 || (op >= 32 && op <= 35) || (op >= 40 && op <= 43) || op == 58) {
			lis &= ~(1u << rd);
		}
		if ((x ^ y) & mask)
			return 0;
	}
	return 1;
}

// A leaf function has no .pdata entry. Its extent in xam: up to padding (a zero word), the next function with
// an entry, or the cap, and at least through its first blr.
static uint32_t leaf_words(uint32_t xam)
{
	uint32_t words = 0;
	int returned = 0;
	while (words < LEAF_MAX_WORDS) {
		const uint32_t address = xam + words * 4;
		if (!in_image(&xam_image, address, 4))
			break;
		const uint32_t word = resolve_read32(address);
		if (word == 0)
			break;
		if (words && function_length(&xam_image, address))
			break;
		if (word == 0x4E800020)
			returned = 1;
		words++;
	}
	return returned && words >= LEAF_MIN_WORDS ? words : 0;
}

static int verify(uint32_t xam, uint32_t length, uint32_t candidate)
{
	if (length) {
		if (!in_image(&dash_image, candidate, length) || function_length(&dash_image, candidate) != length)
			return 0;
		return code_equal(xam, candidate, length / 4);
	}
	const uint32_t words = leaf_words(xam);
	if (!words || !in_image(&dash_image, candidate, words * 4) || function_length(&dash_image, candidate))
		return 0;
	return code_equal(xam, candidate, words);
}

// Every dashboard function with xam's code, up to max; returns the count (max + 1 when there are more).
static uint32_t find_matches(uint32_t xam, uint32_t length, uint32_t *found, uint32_t max)
{
	uint32_t count = 0;
	for (uint32_t i = 0; i < dash_image.pdata_count; i++) {
		if (pdata_length(&dash_image, i) != length)
			continue;
		const uint32_t begin = pdata_begin(&dash_image, i);
		if (!in_image(&dash_image, begin, length) || !code_equal(xam, begin, length / 4))
			continue;
		if (count == max)
			return max + 1;
		found[count++] = begin;
	}
	return count;
}

static void add_anchor(uint32_t ordinal)
{
	if (anchor_count == MAX_ANCHORS)
		return;
	const uint32_t xam = resolve_xam_export(ordinal);
	if (!xam || !in_image(&xam_image, xam, 4))
		return;
	const uint32_t length = function_length(&xam_image, xam);
	// Short functions repeat; they are no use as landmarks.
	if (length < 0x30)
		return;
	uint32_t found[1];
	if (find_matches(xam, length, found, 1) != 1)
		return;
	// Keep the list sorted by xam address.
	int at = anchor_count;
	while (at > 0 && anchors[at - 1].xam > xam) {
		anchors[at] = anchors[at - 1];
		at--;
	}
	anchors[at].xam = xam;
	anchors[at].dash = found[0];
	anchor_count++;
}

static uint32_t by_neighbour(uint32_t xam, uint32_t length)
{
	int below = -1;
	int above = -1;
	for (int i = 0; i < anchor_count; i++) {
		if (anchors[i].xam < xam)
			below = i;
		else if (anchors[i].xam > xam && above < 0)
			above = i;
	}
	uint32_t result = 0;
	const int sides[2] = { below, above };
	for (int s = 0; s < 2; s++) {
		if (sides[s] < 0)
			continue;
		const uint32_t candidate = xam + (anchors[sides[s]].dash - anchors[sides[s]].xam);
		if (!verify(xam, length, candidate))
			continue;
		if (result && result != candidate)
			return 0;  // the two sides disagree
		result = candidate;
	}
	return result;
}

// Functions with identical code (XuiObjectGetProperty and XuiObjectSetProperty differ only in what they call):
// when xam and the dashboard hold the same number of them at the same spacing, they pair up in order.
static uint32_t by_twin(uint32_t xam, uint32_t length)
{
	uint32_t in_dash[MAX_TWINS];
	const uint32_t dash_count = find_matches(xam, length, in_dash, MAX_TWINS);
	if (dash_count < 2 || dash_count > MAX_TWINS)
		return 0;
	uint32_t in_xam[MAX_TWINS];
	uint32_t xam_count = 0;
	uint32_t self = 0;
	for (uint32_t i = 0; i < xam_image.pdata_count; i++) {
		if (pdata_length(&xam_image, i) != length)
			continue;
		const uint32_t begin = pdata_begin(&xam_image, i);
		if (!in_image(&xam_image, begin, length) || !code_equal(xam, begin, length / 4))
			continue;
		if (xam_count == MAX_TWINS)
			return 0;
		if (begin == xam)
			self = xam_count;
		in_xam[xam_count++] = begin;
	}
	if (xam_count != dash_count)
		return 0;
	for (uint32_t i = 1; i < xam_count; i++) {
		if (in_xam[i] - in_xam[i - 1] != in_dash[i] - in_dash[i - 1])
			return 0;
	}
	return in_dash[self];
}

// For a dashboard function xam has no copy of: the address of the one dashboard function that is exactly
// `count` instructions long and equals `words` under `masks`, or 0 when none or several do. Uses the image
// the last resolve_all opened.
uint32_t resolve_find_pattern(const uint32_t *words, const uint32_t *masks, uint32_t count)
{
	uint32_t found = 0;
	for (uint32_t i = 0; i < dash_image.pdata_count; i++) {
		if (pdata_length(&dash_image, i) != count * 4)
			continue;
		const uint32_t begin = pdata_begin(&dash_image, i);
		if (!in_image(&dash_image, begin, count * 4))
			continue;
		uint32_t at = 0;
		while (at < count && !((resolve_read32(begin + at * 4) ^ words[at]) & masks[at]))
			at++;
		if (at < count)
			continue;
		if (found)
			return 0;
		found = begin;
	}
	return found;
}

static int open_images(uint32_t dash_base)
{
	anchor_count = 0;
	for (int i = 0; i < FN_COUNT; i++) {
		resolve_functions[i].dash = 0;
		resolve_functions[i].how = HOW_NONE;
	}

	// xam's image base: the 64 KB boundary at or below an export that starts with a PE header.
	const uint32_t probe = resolve_xam_export(resolve_functions[FN_CREATE_OBJECT].ordinal);
	if (!probe)
		return -1;
	uint32_t base = probe & 0xFFFF0000;
	int opened = -1;
	for (int step = 0; step < 0x200 && opened; step++, base -= 0x10000)
		opened = resolve_open_image(base, &xam_image);
	if (opened)
		return -1;
	if (resolve_open_image(dash_base, &dash_image))
		return -2;
	return 0;
}

// The quick way in: addresses found by resolve_all on an earlier start. Nothing is taken on trust: each one
// must still hold, instruction for instruction, the code of xam's export, exactly as resolve_all demands of
// a candidate. Returns FN_COUNT when all do; anything else means do the full search.
int resolve_from_saved(uint32_t dash_base, const uint32_t *saved)
{
	const int opened = open_images(dash_base);
	if (opened)
		return opened;
	int found = 0;
	for (int i = 0; i < FN_COUNT; i++) {
		ResolveFunction *f = &resolve_functions[i];
		f->xam = resolve_xam_export(f->ordinal);
		if (!f->xam || !in_image(&xam_image, f->xam, 4))
			break;
		f->length = function_length(&xam_image, f->xam);
		if (!saved[i] || !verify(f->xam, f->length, saved[i]))
			break;
		f->dash = saved[i];
		f->how = HOW_SAVED;
		found++;
	}
	const ResolveFunction *focus = &resolve_functions[FN_GET_FOCUS];
	const ResolveFunction *user_focus = &resolve_functions[FN_GET_USER_FOCUS];
	if (found == FN_COUNT && user_focus->dash - focus->dash != user_focus->xam - focus->xam)
		found = 0;
	if (found != FN_COUNT) {
		for (int i = 0; i < FN_COUNT; i++) {
			resolve_functions[i].dash = 0;
			resolve_functions[i].how = HOW_NONE;
		}
	}
	return found;
}

int resolve_all(uint32_t dash_base)
{
	const int opened = open_images(dash_base);
	if (opened)
		return opened;

	// The XUI exports sit in these ordinal runs (Xenia's xam_table.inc).
	for (uint32_t ordinal = 0x320; ordinal <= 0x3FF; ordinal++)
		add_anchor(ordinal);
	for (uint32_t ordinal = 0x807; ordinal <= 0x83F; ordinal++)
		add_anchor(ordinal);
	if (anchor_count < 8)
		return -3;

	int found = 0;
	for (int i = 0; i < FN_COUNT; i++) {
		ResolveFunction *f = &resolve_functions[i];
		f->xam = resolve_xam_export(f->ordinal);
		if (!f->xam || !in_image(&xam_image, f->xam, 4))
			continue;
		f->length = function_length(&xam_image, f->xam);
		if (f->length) {
			uint32_t one[1];
			if (find_matches(f->xam, f->length, one, 1) == 1) {
				f->dash = one[0];
				f->how = HOW_UNIQUE;
			}
		}
		if (!f->dash) {
			f->dash = by_neighbour(f->xam, f->length);
			if (f->dash)
				f->how = HOW_NEIGHBOUR;
		}
		if (!f->dash && f->length) {
			f->dash = by_twin(f->xam, f->length);
			if (f->dash)
				f->how = HOW_TWIN;
		}
		if (f->dash)
			found++;
	}

	// XuiElementGetFocus is three instructions, too few to trust alone: it must sit before
	// XuiElementGetUserFocus exactly as it does in xam.
	ResolveFunction *focus = &resolve_functions[FN_GET_FOCUS];
	const ResolveFunction *user_focus = &resolve_functions[FN_GET_USER_FOCUS];
	if (focus->dash && (!user_focus->dash || user_focus->dash - focus->dash != user_focus->xam - focus->xam)) {
		focus->dash = 0;
		focus->how = HOW_NONE;
		found--;
	}
	return found;
}
