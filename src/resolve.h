// Finds the dashboard's statically linked XUI functions by comparing them with xam.xex's exported copies.
// See resolve.c. Built into the plugin and into the PC test (tools/resolve_test.c, HOST_TEST).
#ifndef WMVSLOP_RESOLVE_H
#define WMVSLOP_RESOLVE_H

#include <stdint.h>

enum {
	FN_CREATE_OBJECT,
	FN_DESTROY_OBJECT,
	FN_ADD_CHILD,
	FN_INSERT_CHILD,
	FN_UNLINK,
	FN_GET_PARENT,
	FN_GET_FIRST_CHILD,
	FN_GET_NEXT,
	FN_GET_ID,
	FN_GET_DESCENDANT_BY_ID,
	FN_SET_BOUNDS,
	FN_SET_OPACITY,
	FN_GET_OBJECT_CLASS,
	FN_GET_PROPERTY_ID,
	FN_SET_PROPERTY,
	FN_GET_FOCUS,
	FN_GET_USER_FOCUS,
	FN_OBJECT_FROM_HANDLE,
	FN_GET_IMAGE_PATH,
	FN_COUNT
};

enum {
	HOW_NONE,       // not found
	HOW_UNIQUE,     // one function in the dashboard has this code
	HOW_NEIGHBOUR,  // several do (or it is a leaf); taken at the same distance from a unique neighbour
	HOW_TWIN,       // several do; same count and spacing as in xam, taken by position
	HOW_SAVED,      // found on an earlier start, and checked again against xam's code on this one
};

typedef struct {
	const char *name;
	uint16_t ordinal;  // xam.xex export ordinal (Xenia's xam_table.inc)
	uint32_t xam;      // address in xam
	uint32_t length;   // bytes from xam's .pdata, 0 for a leaf function
	uint32_t dash;     // address in the dashboard, 0 if not found
	uint8_t how;
} ResolveFunction;

extern ResolveFunction resolve_functions[FN_COUNT];

// Memory access, supplied by the plugin (direct) or the PC test (image files).
uint32_t resolve_read32(uint32_t address);
uint32_t resolve_valid(uint32_t address);
uint32_t resolve_xam_export(uint32_t ordinal);

// Returns the number of functions found, FN_COUNT when all were. dash_base is the dashboard's image base.
// A negative result is a failure before matching: -1 xam's image not found, -2 the dashboard's, -3 no anchors.
int resolve_all(uint32_t dash_base);
int resolve_anchor_count(void);
// The same result from FN_COUNT addresses saved after an earlier resolve_all, each re-checked against xam's
// code; FN_COUNT when all hold, otherwise nothing is set and resolve_all must run.
int resolve_from_saved(uint32_t dash_base, const uint32_t *saved);

// The one dashboard function of exactly `count` instructions equal to `words` under `masks`; 0 if none or
// several. Call after resolve_all.
uint32_t resolve_find_pattern(const uint32_t *words, const uint32_t *masks, uint32_t count);

// The dashboard's "make a video player from a file in memory" function (16197: FUN_927db650): its
// instructions, and the index of its one call. Shared by the plugin and the PC test.
enum { PLAYER_WRAPPER_WORDS = 21, PLAYER_WRAPPER_CALL = 16 };
extern const uint32_t player_wrapper_words[PLAYER_WRAPPER_WORDS];
extern const uint32_t player_wrapper_masks[PLAYER_WRAPPER_WORDS];

// A PE image in memory: 0 on success.
typedef struct {
	uint32_t base;
	uint32_t size;
	uint32_t pdata;        // address of .pdata
	uint32_t pdata_count;  // entries
	uint32_t sections;     // address of the section table
	uint32_t section_count;
} ResolveImage;
int resolve_open_image(uint32_t base, ResolveImage *image);

#endif
