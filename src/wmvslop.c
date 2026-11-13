// WMVSlop: a DashLaunch plugin that plays the .wmv files of a folder behind the Xbox 360 dashboard.
//
// The dashboard already contains a video element, XUI's XuiVideo class (properties File, SizeMode, Pause,
// Loop, Volume; it decodes with the dashboard's own WMV/WMA player, sound included). The plugin creates one
// in the dashboard's element tree and feeds it file names. It runs in two places:
//   - its own system thread: finds the folder, reads the settings, waits for the dashboard, finds the
//     dashboard's XUI functions (resolve.c), installs the VdSwap hook, writes the log;
//   - the hook (wmvslop_tick), on the dashboard's presenting thread once per frame: every XUI call.
// Nothing here is an address from one dashboard build. See ../NOTES.md for what is and isn't verified.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <xsdk/kernel.h>

#include "resolve.h"

uint32_t MmIsAddressValid(const void *address);

void wmvslop_vdswap_hook(void);
void wmvslop_flush_code(void *address);
extern const uint8_t wmvslop_sled[];
// Read by the hook (hook.S).
uint32_t wmvslop_real_vdswap;

void wmvslop_player_hook(void);
// Read by wmvslop_player_hook: the creator it goes on to, and the hardware threads (0-5, two per core) for
// the player's four threads. The dashboard's own choice is 3, 1, 3, 2: two threads on one hardware thread,
// all on the two cores the dashboard itself runs on, the third core unused. These give the two that shared
// hardware thread 3 the third core to themselves.
uint32_t wmvslop_real_create_player;
uint32_t wmvslop_player_cpus[4] = { 4, 1, 5, 2 };

enum {
	DASH_TITLE_ID = 0xFFFE07D1,
	DASH_BASE = 0x92000000,
	VDSWAP_ORDINAL = 0x25B,
	OBJ_CASE_INSENSITIVE = 0x40,
	MAX_VIDEOS = 64,
	NAME_CHARS = 64,
	PATH_CHARS = 160,
	ID_CHARS = 48,
	MAX_HIDE = 4,
	LOG_BYTES = 0x10000,
	DUMP_MAX_NODES = 700,
	DUMP_MAX_DEPTH = 8,
	// XUI_ELEMENT_PROPERTY_TYPE, as XuiVideo's property table uses them (decomp/video2.c).
	PROP_BOOL = 1,
	PROP_UNSIGNED = 3,
	PROP_FLOAT = 4,
	PROP_STRING = 5,
	// XuiVideo's SizeMode bit that stretches the picture to the element's bounds (decomp/video3.c, FUN_927ccb40).
	SIZE_MODE_STRETCH = 4,
	// Word of XuiVideo's instance naming the frame buffer on screen (decomp/video3.c, FUN_927ccfe8).
	VIDEO_BUFFER_WORD = 0xD,
};

// ---------------------------------------------------------------------------------------------------------
// Small runtime: the plugin links no C library.

void *memset(void *destination, int value, size_t bytes)
{
	volatile uint8_t *d = destination;
	while (bytes--)
		*d++ = (uint8_t)value;
	return destination;
}

void *memcpy(void *destination, const void *source, size_t bytes)
{
	volatile uint8_t *d = destination;
	const uint8_t *s = source;
	while (bytes--)
		*d++ = *s++;
	return destination;
}

static size_t text_length(const char *text)
{
	size_t n = 0;
	while (text[n])
		n++;
	return n;
}

static void text_copy(char *destination, size_t capacity, const char *source)
{
	size_t n = 0;
	while (source[n] && n + 1 < capacity) {
		destination[n] = source[n];
		n++;
	}
	destination[n] = 0;
}

static void text_append(char *destination, size_t capacity, const char *source)
{
	const size_t used = text_length(destination);
	text_copy(destination + used, capacity - used, source);
}

static char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int text_compare_nocase(const char *a, const char *b)
{
	while (*a && lower(*a) == lower(*b)) {
		a++;
		b++;
	}
	return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

static void to_wide(uint16_t *destination, size_t capacity, const char *source)
{
	size_t n = 0;
	while (source[n] && n + 1 < capacity) {
		destination[n] = (uint8_t)source[n];
		n++;
	}
	destination[n] = 0;
}

static void sleep_ms(uint32_t ms)
{
	// Relative time, in 100 ns units.
	const int64_t interval = -(int64_t)ms * 10000;
	KeDelayExecutionThread(0, 0, &interval);
}

// ---------------------------------------------------------------------------------------------------------
// Log: both threads append to memory; the system thread writes it to WMVSlop.log in the folder.

static char log_text[LOG_BYTES];
static volatile uint32_t log_used;
static uint32_t log_written;
static volatile uint32_t log_lock;

static void log_raw(const char *text);
static void log_number_raw(uint32_t value);

// Takes the log and starts an entry with the time since the plugin started, as seconds.tenths: the user
// asked why the console took long to come up, and a log without times can't say which part did.
static void log_lock_only(void)
{
	while (__sync_lock_test_and_set(&log_lock, 1))
		;
}

static void log_take(void)
{
	log_lock_only();
	static uint64_t started;
	uint64_t now = 0;
	KeQuerySystemTime(&now);
	if (!started)
		started = now;
	// 100 ns units.
	const uint32_t tenths = (uint32_t)((now - started) / 1000000);
	log_raw("[");
	log_number_raw(tenths / 10);
	log_raw(".");
	log_number_raw(tenths % 10);
	log_raw("] ");
}

static void log_give(void)
{
	__sync_lock_release(&log_lock);
}

static void log_raw(const char *text)
{
	while (*text && log_used + 1 < LOG_BYTES)
		log_text[log_used++] = *text++;
}

static void log_hex_raw(uint32_t value)
{
	char digits[9];
	for (int i = 0; i < 8; i++) {
		const uint32_t nibble = value >> (28 - i * 4) & 15;
		digits[i] = (char)(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
	}
	digits[8] = 0;
	log_raw(digits);
}

static void log_number_raw(uint32_t value)
{
	char digits[11];
	int n = 10;
	digits[n] = 0;
	do {
		digits[--n] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	log_raw(digits + n);
}

// One line: text, then an optional value in hex.
static void log_line(const char *text)
{
	log_take();
	log_raw(text);
	log_raw("\r\n");
	log_give();
}

static void log_value(const char *text, uint32_t value)
{
	log_take();
	log_raw(text);
	log_raw(" ");
	log_hex_raw(value);
	log_raw("\r\n");
	log_give();
}

static void log_text_value(const char *text, const char *value)
{
	log_take();
	log_raw(text);
	log_raw(" ");
	log_raw(value);
	log_raw("\r\n");
	log_give();
}

// ---------------------------------------------------------------------------------------------------------
// Files (system thread only). Paths are kernel device paths.

static NTSTATUS file_open(HANDLE *handle, const char *path, uint32_t access, uint32_t disposition, bool directory)
{
	const ANSI_STRING name = { .length = (uint16_t)text_length(path),
		.maximum_length = (uint16_t)(text_length(path) + 1), .pointer = path };
	const OBJECT_ATTRIBUTES attributes = { .root_directory = 0, .name = &name, .attributes = OBJ_CASE_INSENSITIVE };
	IO_STATUS_BLOCK io_status;
	return NtCreateFile(handle, access | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE, disposition,
		FILE_SYNCHRONOUS_IO_NONALERT | (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE));
}

// Returns the bytes read, or -1.
static int32_t file_read_all(const char *path, void *buffer, uint32_t capacity)
{
	HANDLE handle;
	if (!NT_SUCCESS(file_open(&handle, path, GENERIC_READ, FILE_OPEN, false)))
		return -1;
	IO_STATUS_BLOCK io_status = { 0 };
	const uint64_t offset = 0;
	const NTSTATUS status = NtReadFile(handle, 0, NULL, NULL, &io_status, buffer, capacity, &offset);
	NtClose(handle);
	return NT_SUCCESS(status) ? (int32_t)io_status.information : -1;
}

static bool file_write_at(const char *path, const void *data, uint32_t bytes, uint64_t offset, bool create)
{
	HANDLE handle;
	if (!NT_SUCCESS(file_open(&handle, path, GENERIC_WRITE, create ? FILE_OVERWRITE_IF : FILE_OPEN_IF, false)))
		return false;
	IO_STATUS_BLOCK io_status = { 0 };
	const NTSTATUS status = NtWriteFile(handle, 0, NULL, NULL, &io_status, data, bytes, &offset);
	NtClose(handle);
	return NT_SUCCESS(status);
}

// ---------------------------------------------------------------------------------------------------------
// Settings and playlist.

static struct {
	char folder[PATH_CHARS];          // device path of the WMVSlop folder, no trailing backslash
	char videos[MAX_VIDEOS][NAME_CHARS];
	uint32_t lengths_ms[MAX_VIDEOS];  // each video's playing time, 0 if unknown
	uint32_t margins_ms[MAX_VIDEOS];  // how long before its end each is left for the next
	uint32_t video_count;
	bool enabled;                     // video=0 only writes the log
	bool dump;                        // write the dashboard's element tree to the log
	bool front;                       // add above the parent's other children instead of below them
	uint32_t auto_place;              // 1 over the dashboard's whole background, 2 inside it, 3 behind everything
	                                  // with the background hidden, 0 bottom of the root
	uint32_t width;
	uint32_t height;
	uint32_t delay_frames;            // frames of dashboard before the video is created
	bool volume_set;
	int32_t volume_db;                // XuiVideo's Volume, -96 to 6
	char attach[ID_CHARS];            // element ID to attach under; empty is the root
	char hide[MAX_HIDE][ID_CHARS];    // element IDs to make transparent
	uint32_t hide_count;
	bool spread;                      // move the player's threads onto the idle core
	bool stream;                      // play files straight from the stick instead of loading them whole
} settings;

static const char *const DEVICES[] = {
	"\\Device\\Mass0",
	"\\Device\\Mass1",
	"\\Device\\Mass2",
	"\\Device\\Harddisk0\\Partition1",
};

static bool find_folder(void)
{
	for (size_t i = 0; i < sizeof(DEVICES) / sizeof(DEVICES[0]); i++) {
		char path[PATH_CHARS];
		text_copy(path, sizeof(path), DEVICES[i]);
		text_append(path, sizeof(path), "\\WMVSlop");
		HANDLE handle;
		if (NT_SUCCESS(file_open(&handle, path, GENERIC_READ, FILE_OPEN, true))) {
			NtClose(handle);
			text_copy(settings.folder, sizeof(settings.folder), path);
			return true;
		}
	}
	return false;
}

static void folder_path(char *path, size_t capacity, const char *name)
{
	text_copy(path, capacity, settings.folder);
	text_append(path, capacity, "\\");
	text_append(path, capacity, name);
}

static bool ends_with_wmv(const char *name, uint32_t length)
{
	return length > 4 && name[length - 4] == '.' && lower(name[length - 3]) == 'w' && lower(name[length - 2]) == 'm'
		&& lower(name[length - 1]) == 'v';
}

// A listing of the folder's .wmv files, in playing order. The signature changes when a file is added, removed,
// renamed or replaced by one of another size.
typedef struct {
	char videos[MAX_VIDEOS][NAME_CHARS];
	uint32_t lengths_ms[MAX_VIDEOS];  // playing time from each file's header, 0 if it couldn't be read
	uint32_t margins_ms[MAX_VIDEOS];  // how long before its end each file must be left
	uint32_t video_count;
	uint32_t signature;
} Listing;

static void folder_path(char *path, size_t capacity, const char *name);

static uint64_t little_endian_64(const uint8_t *bytes)
{
	uint64_t value = 0;
	for (int i = 7; i >= 0; i--)
		value = value << 8 | bytes[i];
	return value;
}

// A .wmv file's playing time in milliseconds, from its ASF File Properties object: after the object's 16-byte
// ID come its size (8), file ID (16), file size (8), creation date (8), packet count (8), then the play
// duration in 100 ns units (8), the send duration (8) and the preroll in ms (8); the play duration includes the
// preroll. Checked against ffprobe on the user's files by tools/asflength.py. 0 if the object isn't found
// in the first 8 KB or the value makes no sense.
static uint32_t video_length_ms(const char *name, uint32_t *margin_ms)
{
	*margin_ms = 0;
	static const uint8_t FILE_PROPERTIES[16] = { 0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
		0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 };
	static uint8_t head[8192];
	char path[PATH_CHARS];
	folder_path(path, sizeof(path), name);
	const int32_t bytes = file_read_all(path, head, sizeof(head));
	for (int32_t at = 0; at + 104 <= bytes; at++) {
		int same = 0;
		while (same < 16 && head[at + same] == FILE_PROPERTIES[same])
			same++;
		if (same < 16)
			continue;
		const uint64_t play = little_endian_64(head + at + 64) / 10000;
		const uint64_t preroll = little_endian_64(head + at + 80);
		// Between two seconds and a day.
		if (play <= preroll || play - preroll < 2000 || play - preroll > 86400000)
			return 0;
		const uint32_t length = (uint32_t)(play - preroll);
		// How long before the end the plugin must leave the file (see follow_playlist): the time the last
		// 2 MB take to play at the file's average rate, plus a second and a half. The player reads about
		// 1 MB ahead of what it shows (8 buffers of 128 KB, decomp/player2.c), and it has to be let go before
		// that reading reaches the end of the file. At least 3 s, at most half the video.
		const uint64_t file_bytes = little_endian_64(head + at + 40);
		uint64_t margin = file_bytes ? (uint64_t)length * 0x200000 / file_bytes + 1500 : 3000;
		if (margin < 3000)
			margin = 3000;
		if (margin > length / 2)
			margin = length / 2;
		*margin_ms = (uint32_t)margin;
		return length;
	}
	return 0;
}

// Fills in the lengths of a listing's files. Reads 8 KB of each, so only for a listing about to be used.
static void measure_videos(Listing *listing)
{
	for (uint32_t i = 0; i < listing->video_count; i++)
		listing->lengths_ms[i] = video_length_ms(listing->videos[i], &listing->margins_ms[i]);
}

// False when the folder can't be opened (the stick is out).
static bool list_videos(Listing *listing)
{
	listing->video_count = 0;
	listing->signature = 0;
	HANDLE handle;
	if (!NT_SUCCESS(file_open(&handle, settings.folder, GENERIC_READ, FILE_OPEN, true)))
		return false;
	static const ANSI_STRING ALL_FILES = { .length = 1, .maximum_length = 2, .pointer = "*" };
	bool restart = true;
	for (;;) {
		// One entry per call: a FAT32 stick's driver doesn't chain entries (xsdk docs/HARDWARE.md).
		static union {
			FILE_DIRECTORY_INFORMATION information;
			uint8_t bytes[sizeof(FILE_DIRECTORY_INFORMATION) + 264];
		} entry;
		memset(&entry, 0, sizeof(entry));
		IO_STATUS_BLOCK io_status = { 0 };
		const NTSTATUS status = NtQueryDirectoryFile(handle, 0, NULL, NULL, &io_status, &entry.information,
			sizeof(FILE_DIRECTORY_INFORMATION) + 256, restart ? &ALL_FILES : NULL, restart);
		restart = false;
		if (!NT_SUCCESS(status))
			break;
		const uint32_t length = entry.information.file_name_length;
		if (length >= NAME_CHARS || (entry.information.attributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		if (!ends_with_wmv(entry.information.file_name, length) || listing->video_count == MAX_VIDEOS)
			continue;
		char *name = listing->videos[listing->video_count++];
		memcpy(name, entry.information.file_name, length);
		name[length] = 0;
		// A sum, so the order the driver lists files in doesn't matter.
		uint32_t hash = 2166136261u;
		for (uint32_t i = 0; i < length; i++)
			hash = (hash ^ (uint8_t)lower(name[i])) * 16777619u;
		listing->signature += (hash ^ (uint32_t)entry.information.end_of_file) * 2654435761u + 1;
	}
	NtClose(handle);

	// Play in name order.
	for (uint32_t i = 1; i < listing->video_count; i++) {
		char moving[NAME_CHARS];
		text_copy(moving, sizeof(moving), listing->videos[i]);
		uint32_t at = i;
		while (at > 0 && text_compare_nocase(listing->videos[at - 1], moving) > 0) {
			text_copy(listing->videos[at], NAME_CHARS, listing->videos[at - 1]);
			at--;
		}
		text_copy(listing->videos[at], NAME_CHARS, moving);
	}
	return true;
}

// The folder as last looked at by the system thread. While scan_pending is set it belongs to the dashboard's
// thread, which takes it over at the top of its next frame (wmvslop_tick).
static Listing scan;
static volatile uint32_t scan_pending;
static uint32_t live_signature;
static uint32_t ignored_videos;  // .wmv files in the folder beyond the one that is played

// Makes the scanned listing the playlist.
static void take_listing(void)
{
	memcpy(settings.videos, scan.videos, sizeof(settings.videos));
	memcpy(settings.lengths_ms, scan.lengths_ms, sizeof(settings.lengths_ms));
	memcpy(settings.margins_ms, scan.margins_ms, sizeof(settings.margins_ms));
	// One video, looping: the user's decision on 2026-11-13, after playlists kept ending in crashes on a
	// friend's console ("forget 2 videos, make sure it uses 1 wmv and loops"). With several .wmv files in the
	// folder the first by name is the one played and the rest are ignored. The playlist code below it
	// (lengths, follow_playlist's leave-early branch) is still there but can't be reached with one video.
	settings.video_count = scan.video_count ? 1 : 0;
	ignored_videos = scan.video_count > 1 ? scan.video_count - 1 : 0;
	live_signature = scan.signature;
}

static void log_listing(void)
{
	if (!settings.video_count)
		log_line("no .wmv file in the folder");
	if (ignored_videos)
		log_value("more than one .wmv in the folder; only the first by name is played. Others ignored:", ignored_videos);
	for (uint32_t i = 0; i < settings.video_count; i++) {
		log_take();
		log_raw("video: ");
		log_raw(settings.videos[i]);
		log_raw(", ms ");
		log_number_raw(settings.lengths_ms[i]);
		log_raw(", left this many ms before its end: ");
		log_number_raw(settings.margins_ms[i]);
		log_raw("\r\n");
		log_give();
	}
}

static int32_t parse_number(const char *text)
{
	int32_t sign = 1;
	if (*text == '-') {
		sign = -1;
		text++;
	}
	int32_t value = 0;
	while (*text >= '0' && *text <= '9')
		value = value * 10 + (*text++ - '0');
	return sign * value;
}

static void apply_setting(const char *key, const char *value)
{
	if (!text_compare_nocase(key, "video"))
		settings.enabled = parse_number(value) != 0;
	else if (!text_compare_nocase(key, "dump"))
		settings.dump = parse_number(value) != 0;
	else if (!text_compare_nocase(key, "front"))
		settings.front = parse_number(value) != 0;
	else if (!text_compare_nocase(key, "spread"))
		settings.spread = parse_number(value) != 0;
	else if (!text_compare_nocase(key, "stream"))
		settings.stream = parse_number(value) != 0;
	else if (!text_compare_nocase(key, "auto"))
		settings.auto_place = (uint32_t)parse_number(value);
	else if (!text_compare_nocase(key, "width"))
		settings.width = (uint32_t)parse_number(value);
	else if (!text_compare_nocase(key, "height"))
		settings.height = (uint32_t)parse_number(value);
	else if (!text_compare_nocase(key, "delay"))
		settings.delay_frames = (uint32_t)parse_number(value);
	else if (!text_compare_nocase(key, "volume")) {
		settings.volume_set = true;
		settings.volume_db = parse_number(value);
	} else if (!text_compare_nocase(key, "attach"))
		text_copy(settings.attach, sizeof(settings.attach), value);
	else if (!text_compare_nocase(key, "hide") && settings.hide_count < MAX_HIDE && *value)
		text_copy(settings.hide[settings.hide_count++], ID_CHARS, value);
}

// The ini's size as read at start, 0 if there is none (used for streaming, see play).
static uint32_t placeholder_bytes;

// WMVSlop.ini: lines of key=value; anything else is ignored.
static void read_settings(void)
{
	settings.enabled = true;
	settings.dump = false;
	settings.front = false;
	// 1, the place 0.11-0.16 used and the user called working. 3 (behind everything, background hidden) played
	// through a Live sign-in in one 0.17 run, but the starts after it had no video and a crash, cause unknown.
	settings.auto_place = 1;
	settings.width = 1280;
	settings.height = 720;
	settings.delay_frames = 60;
	settings.volume_set = false;
	settings.attach[0] = 0;
	settings.hide_count = 0;
	// Off unless asked for. 0.12/0.13 had it on, and on the user's console profiles then signed in without
	// reaching Xbox Live (sign-in state 1, then out, then 1 again, where 0.6-0.10 logs show 2). Not proven to
	// be the cause, but the dashboard's own choice keeps the player off hardware threads 0, 4 and 5, and
	// overriding that is the one thing in those versions that takes processor time from the rest of the system.
	settings.spread = false;
	settings.stream = true;

	char path[PATH_CHARS];
	folder_path(path, sizeof(path), "WMVSlop.ini");
	static char text[8192];
	const int32_t bytes = file_read_all(path, text, sizeof(text) - 1);
	// The ini doubles as the small file XuiVideo "loads" when a video is streamed (see play); its size is how
	// the plugin knows that request. A file that fills the buffer may be longer than was read: no streaming.
	placeholder_bytes = bytes > 0 && bytes < (int32_t)sizeof(text) - 1 ? (uint32_t)bytes : 0;
	if (bytes <= 0)
		return;
	text[bytes] = 0;
	char *line = text;
	while (*line) {
		char *end = line;
		while (*end && *end != '\r' && *end != '\n')
			end++;
		char *next = end;
		while (*next == '\r' || *next == '\n')
			next++;
		*end = 0;
		char *equals = line;
		while (*equals && *equals != '=')
			equals++;
		if (*equals == '=' && *line != '#' && *line != ';') {
			*equals = 0;
			apply_setting(line, equals + 1);
		}
		line = next;
	}
}

// ---------------------------------------------------------------------------------------------------------
// The dashboard side. Everything from here to the system thread runs in wmvslop_tick, on the dashboard's thread.

typedef uint32_t HXUIOBJ;

#define FUNCTION(index, type) ((type)(uintptr_t)resolve_functions[index].dash)

static int32_t xui_create_object(const uint16_t *class_name, HXUIOBJ *object)
{
	return FUNCTION(FN_CREATE_OBJECT, int32_t (*)(const uint16_t *, HXUIOBJ *))(class_name, object);
}

static int32_t xui_destroy_object(HXUIOBJ object)
{
	return FUNCTION(FN_DESTROY_OBJECT, int32_t (*)(HXUIOBJ))(object);
}

static int32_t xui_add_child(HXUIOBJ parent, HXUIOBJ child)
{
	return FUNCTION(FN_ADD_CHILD, int32_t (*)(HXUIOBJ, HXUIOBJ))(parent, child);
}

// With no neighbours given the child becomes the parent's first, the one drawn first (decomp/insert.c).
static int32_t xui_insert_first(HXUIOBJ parent, HXUIOBJ child)
{
	return FUNCTION(FN_INSERT_CHILD, int32_t (*)(HXUIOBJ, HXUIOBJ, HXUIOBJ, HXUIOBJ))(parent, child, 0, 0);
}

static HXUIOBJ xui_relative(int function, HXUIOBJ object)
{
	HXUIOBJ result = 0;
	const int32_t status = FUNCTION(function, int32_t (*)(HXUIOBJ, HXUIOBJ *))(object, &result);
	return status >= 0 ? result : 0;
}

static const uint16_t *xui_id(HXUIOBJ object)
{
	const uint16_t *id = NULL;
	const int32_t status = FUNCTION(FN_GET_ID, int32_t (*)(HXUIOBJ, const uint16_t **))(object, &id);
	return status >= 0 ? id : NULL;
}

static HXUIOBJ xui_descendant(HXUIOBJ root, const char *id)
{
	uint16_t wide[ID_CHARS];
	to_wide(wide, ID_CHARS, id);
	HXUIOBJ result = 0;
	const int32_t status = FUNCTION(FN_GET_DESCENDANT_BY_ID,
		int32_t (*)(HXUIOBJ, const uint16_t *, HXUIOBJ *, uint32_t))(root, wide, &result, 0);
	return status >= 0 ? result : 0;
}

// A property value: its type, then the value (decomp/video3.c, the File and SizeMode setters). The real
// structure is larger for vectors; the padding covers it.
typedef struct {
	uint32_t type;
	uint32_t value;
	uint32_t padding[6];
} PropertyValue;

static int32_t xui_set_property(HXUIOBJ object, const char *name, uint32_t type, uint32_t value)
{
	uint16_t wide[ID_CHARS];
	to_wide(wide, ID_CHARS, name);
	uint32_t id = 0;
	int32_t status = FUNCTION(FN_GET_PROPERTY_ID, int32_t (*)(HXUIOBJ, const uint16_t *, uint32_t *))(object, wide, &id);
	if (status < 0)
		return status;
	PropertyValue property = { .type = type, .value = value };
	return FUNCTION(FN_SET_PROPERTY, int32_t (*)(HXUIOBJ, uint32_t, uint32_t, const PropertyValue *))(object, id, 0,
		&property);
}

static bool readable(const void *address, uint32_t bytes)
{
	const uintptr_t first = (uintptr_t)address;
	return first >= 0x10000 && MmIsAddressValid(address) && MmIsAddressValid((const void *)(first + bytes - 1));
}

// Appends a wide string the dashboard owns, if it reads as plain text. Caller holds the log.
static void log_wide_raw(const uint16_t *text)
{
	if (!text || !readable(text, 2)) {
		log_raw("-");
		return;
	}
	char narrow[64];
	size_t n = 0;
	while (n + 1 < sizeof(narrow)) {
		if (!readable(text + n, 2))
			break;
		const uint16_t c = text[n];
		if (!c)
			break;
		narrow[n++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
	}
	narrow[n] = 0;
	log_raw(n ? narrow : "-");
}

// The class handle is XUI's own record of the class; in the builds looked at its second word is the name.
// Read defensively: it is only for the log.
static const uint16_t *class_name(HXUIOBJ object)
{
	const uint32_t *class = (const uint32_t *)(uintptr_t)FUNCTION(FN_GET_OBJECT_CLASS, uint32_t (*)(HXUIOBJ))(object);
	if (!class || !readable(class, 8))
		return NULL;
	const uint16_t *name = (const uint16_t *)(uintptr_t)class[1];
	if (!readable(name, 2) || name[0] < 'A' || name[0] > 'z')
		return NULL;
	return name;
}

static bool name_has(const uint16_t *name, const char *part)
{
	if (!name)
		return false;
	for (size_t at = 0; at < 48 && name[at]; at++) {
		size_t n = 0;
		while (part[n] && name[at + n] == (uint8_t)part[n])
			n++;
		if (!part[n])
			return true;
	}
	return false;
}

static uint32_t dump_nodes;

static void dump_tree(HXUIOBJ object, uint32_t depth)
{
	while (object && dump_nodes < DUMP_MAX_NODES) {
		dump_nodes++;
		const uint16_t *class = class_name(object);
		log_take();
		for (uint32_t i = 0; i < depth; i++)
			log_raw("  ");
		log_hex_raw(object);
		log_raw(" ");
		log_wide_raw(class);
		log_raw(" id=");
		log_wide_raw(xui_id(object));
		if (name_has(class, "Image")) {
			const uint16_t *path = NULL;
			if (FUNCTION(FN_GET_IMAGE_PATH, int32_t (*)(HXUIOBJ, const uint16_t **))(object, &path) >= 0 && path) {
				log_raw(" image=");
				log_wide_raw(path);
			}
		}
		log_raw("\r\n");
		log_give();
		if (depth < DUMP_MAX_DEPTH)
			dump_tree(xui_relative(FN_GET_FIRST_CHILD, object), depth + 1);
		object = xui_relative(FN_GET_NEXT, object);
	}
}

enum { STAGE_WAITING, STAGE_PLAYING, STAGE_FAILED };

static struct {
	volatile uint32_t armed;   // set by the system thread once the hook may touch the dashboard
	uint32_t frames;
	uint32_t stage;
	uint32_t attempts;
	uint32_t step;             // setup's next step
	uint32_t step_frame;       // frame the last step ran on
	uint32_t step_gap;         // frames to leave before the next step
	uint32_t rebuilding;       // the video is being put back, not made for the first time
	HXUIOBJ root;
	HXUIOBJ parent;
	HXUIOBJ after;             // sibling the video goes after, 0 for first or last child
	HXUIOBJ video;
	uint32_t video_class;      // XuiVideo's class handle, read from the element when it was made
	uint32_t signin;           // signin_mask() when the video was made or last rebuilt
	uint32_t rebuild_frame;    // frame to rebuild the video on after a sign-in change, 0 for none
	uint32_t current;
	uint32_t last_buffer;      // the instance's buffer word when last looked at
	uint32_t buffer_frame;     // frame it last changed on
	uint32_t moving;           // the current file has shown a frame
	uint32_t streaming;        // the current file was asked for through the placeholder
	uint32_t loop_on;          // the element was made with Loop on (the player restarts the file itself)
	uint64_t first_frame_time; // KeQuerySystemTime when the current file showed its first frame
	uint32_t hide_aura;        // auto=3: keep the dashboard's background picture invisible
	HXUIOBJ hidden_aura;       // the picture element last hidden
	uint32_t created;          // the first file was set
} dash;

// Setup runs as separate steps a second apart, so the system thread writes each step's log lines to the
// stick before the next one runs: if a step freezes the console, the log ends at that step.
enum { STEP_FIND, STEP_CREATE, STEP_INSERT, STEP_PLAY, STEP_FRAMES_APART = 30, RETRY_FRAMES_APART = 15 };
enum { SETUP_RETRY, SETUP_NEXT, SETUP_DONE };

static const char LINK_NAME[] = "\\??\\WMVSlop:";

// XamUserGetSigninState, looked up by the system thread; NULL if xam doesn't have it.
static uint32_t (*signin_state)(uint32_t user_index);

// The four controllers' sign-in states (0 none, 1 local, 2 Xbox Live) in one number.
static uint32_t signin_mask(void)
{
	uint32_t mask = 0;
	for (uint32_t user = 0; user < 4; user++)
		mask |= (signin_state(user) & 3) << (user * 4);
	return mask;
}

// Streaming. XuiVideo itself only knows one way to play a file: read all of it into memory, then make a player
// over that memory (decomp/video3.c, FUN_927cd5e8). The dashboard's player can also read a file piece by piece
// (parameter block type 1: a file name, opened with CreateFileA; decomp/player2.c, source3.c). To get that
// without touching XuiVideo, the element is given a small file to "load" in place of the video, and when its
// request for a player over that file's bytes comes through wmvslop_player_hook, the request is changed into
// one for a player over the real file. No size limit, no long read before the first frame, and the video's
// memory stays free.
//
// The small file is WMVSlop.ini, which is in the folder anyway. 0.13-0.17 wrote a WMVSlop.stub for this; the
// user asked for the plugin not to need extra files (2026-11-13). Without an ini, videos are loaded whole.
static const char PLACEHOLDER_NAME[] = "WMVSlop.ini";
// The file the next request stands for, as CreateFileA takes it. Each play() uses the next of eight slots:
// the name is handed to the player as a pointer, and whether the player keeps that pointer isn't known, so a
// name is not overwritten while a player made with it can still be around.
static char stream_names[8][PATH_CHARS];
static uint32_t stream_slot;
#define stream_name (stream_names[stream_slot % 8])
static uint32_t stream_pending;       // play() has asked for the placeholder and no player has been made yet
static uint32_t player_hooked;        // this dashboard's player requests come through the hook
static uint32_t stream_failed;        // streaming gave no picture on this dashboard; files are loaded whole

// Called by wmvslop_player_hook with the dashboard's parameter block for a new player (layout: hook.S).
void wmvslop_adjust_player(uint32_t *block)
{
	if (settings.spread) {
		block[1] |= 2;
		for (int i = 0; i < 4; i++)
			block[2 + i] = wmvslop_player_cpus[i];
	}
	// Type 3: a buffer at word 8 and its size at word 9. The request is the plugin's when one is expected and
	// the buffer is as long as the placeholder; any other player the dashboard makes is left as it is.
	if (block[0] != 3 || !stream_pending || !placeholder_bytes || block[9] != placeholder_bytes)
		return;
	stream_pending = 0;
	// Type 1: a file name at word 8; words 9-11 zero take the player's own read buffer size and count.
	block[0] = 1;
	block[8] = (uint32_t)(uintptr_t)stream_name;
	block[9] = 0;
	log_text_value("player asked to stream", stream_name);
}

static void play(uint32_t index)
{
	// The dashboard's file names are XUI locators over Win32 drive names; WMVSlop: is the link made in setup.
	char path[PATH_CHARS];
	text_copy(path, sizeof(path), "file://WMVSlop:\\");
	dash.streaming = settings.stream && player_hooked && !stream_failed && placeholder_bytes;
	stream_pending = dash.streaming;
	if (dash.streaming) {
		stream_slot++;
		text_copy(stream_name, PATH_CHARS, "WMVSlop:\\");
		text_append(stream_name, PATH_CHARS, settings.videos[index]);
		text_append(path, sizeof(path), PLACEHOLDER_NAME);
	} else {
		text_append(path, sizeof(path), settings.videos[index]);
	}
	static uint16_t wide[PATH_CHARS];
	to_wide(wide, PATH_CHARS, path);
	const int32_t status = xui_set_property(dash.video, "File", PROP_STRING, (uint32_t)(uintptr_t)wide);
	log_take();
	log_raw(dash.streaming ? "play (streamed) " : "play (loaded whole) ");
	log_raw(settings.videos[index]);
	log_raw(" -> ");
	log_hex_raw((uint32_t)status);
	log_raw("\r\n");
	log_give();
	dash.current = index;
	dash.last_buffer = 0xFFFFFFFF;
	dash.buffer_frame = dash.frames;
	dash.moving = 0;
}

// Whether an element's ID is exactly this text.
static bool id_is(HXUIOBJ object, const char *text)
{
	const uint16_t *id = xui_id(object);
	if (!id || !readable(id, 2))
		return false;
	size_t n = 0;
	while (text[n]) {
		if (!readable(id + n, 2) || id[n] != (uint8_t)text[n])
			return false;
		n++;
	}
	return readable(id + n, 2) && id[n] == 0;
}

// The dashboard's background picture element: the direct child of "backgroundScene" whose ID is exactly
// "Aura" (the home hub has another element of that name deeper down, so no search by ID).
static HXUIOBJ find_aura_image(HXUIOBJ root, HXUIOBJ *background)
{
	*background = xui_descendant(root, "backgroundScene");
	for (HXUIOBJ child = *background ? xui_relative(FN_GET_FIRST_CHILD, *background) : 0; child;
		child = xui_relative(FN_GET_NEXT, child)) {
		if (id_is(child, "Aura"))
			return child;
	}
	return 0;
}

// auto=3 keeps the dashboard's own background picture invisible, so the video behind everything shows.
// Called six times a second: a Live sign-in gives the dashboard a new backgroundScene with a new, visible
// picture, and the dashboard may set the opacity back itself.
static void keep_aura_hidden(float opacity)
{
	HXUIOBJ background = 0;
	const HXUIOBJ aura_image = find_aura_image(dash.root, &background);
	if (!aura_image)
		return;
	FUNCTION(FN_SET_OPACITY, int32_t (*)(HXUIOBJ, float))(aura_image, opacity);
	if (aura_image != dash.hidden_aura) {
		dash.hidden_aura = aura_image;
		log_value(opacity == 0.0f ? "dashboard's background picture hidden:" : "dashboard's background picture shown:",
			aura_image);
	}
}

// Step 1: the root element and where the video goes.
static int setup_find(void)
{
	// The focused element exists once the dashboard's first screen is up; its topmost ancestor is the root.
	HXUIOBJ root = FUNCTION(FN_GET_FOCUS, HXUIOBJ (*)(void))();
	if (!root)
		return SETUP_RETRY;
	for (int depth = 0; depth < 64; depth++) {
		const HXUIOBJ parent = xui_relative(FN_GET_PARENT, root);
		if (!parent)
			break;
		root = parent;
	}
	// Early in the dashboard's start the background scene isn't built yet. Wait for it (80 tries, about 20 s)
	// before settling for the bottom of the root, which is all another system app (the avatar editor) offers.
	const bool want_aura = settings.auto_place && !settings.front && !settings.attach[0];
	// auto=1 and 3: the picture element the whole background lives in, a direct child of "backgroundScene".
	HXUIOBJ background = 0;
	HXUIOBJ aura_image = 0;
	if (want_aura && (settings.auto_place == 1 || settings.auto_place == 3))
		aura_image = find_aura_image(root, &background);
	if (want_aura && dash.attempts < 80 && !aura_image && !xui_descendant(root, "AuraScene"))
		return SETUP_RETRY;
	log_value("step 1: root element", root);
	if (settings.dump) {
		log_line("-- element tree (handle, class, id) --");
		dump_nodes = 0;
		dump_tree(root, 0);
		log_value("-- end of tree, elements listed:", dump_nodes);
	}
	if (!settings.enabled || !settings.video_count) {
		log_line("no video created (video=0 or no .wmv files)");
		return SETUP_DONE;
	}

	// The dashboard opens files through its own drive names, so the folder gets one. This runs on the
	// dashboard's thread on purpose: \?? is the title's name space there.
	const ANSI_STRING link = { .length = sizeof(LINK_NAME) - 1, .maximum_length = sizeof(LINK_NAME),
		.pointer = LINK_NAME };
	const ANSI_STRING target = { .length = (uint16_t)text_length(settings.folder),
		.maximum_length = (uint16_t)(text_length(settings.folder) + 1), .pointer = settings.folder };
	ObDeleteSymbolicLink(&link);
	log_value("drive link WMVSlop: ->", (uint32_t)ObCreateSymbolicLink(&link, &target));

	dash.root = root;
	dash.parent = root;
	dash.after = 0;
	if (settings.attach[0]) {
		const HXUIOBJ found = xui_descendant(root, settings.attach);
		log_text_value(found ? "attach under" : "attach element not found, using the root:", settings.attach);
		if (found)
			dash.parent = found;
	}
	// Where the background is (the user's dash 17559 tree, NOTES.md):
	//   root > backgroundScene > [ XuiImage "Aura" > canvas > AuraScene > Back, Theme, Sphere, Banner,
	//                              vignettes, snow ], overlays, cursor, DashAppHostScene (the tiles), legend
	// Signing in to Xbox Live makes the dashboard throw AuraScene away and build a new one inside the same
	// "Aura" image (AuraScene's handle changes in every log; the image's is from the first batch made).
	// auto=1 puts the video in backgroundScene right after that image: over the whole background, under the
	// tiles, and not inside anything sign-in destroys, so it keeps playing through a sign-in.
	// auto=2 is the 0.2-0.10 place, inside AuraScene after Theme: under the dashboard's vignettes, but it is
	// destroyed and made again (the clip restarts) at every sign-in.
	// auto=3 puts the video where 0.1 had it, the root's first child, behind backgroundScene and everything in
	// it, and hides the Aura image so it shows (0.1 was visible there until a theme's wallpaper covered it).
	// The root is the one element no log has ever shown replaced, so the video plays on through a Live
	// sign-in. The cost: the dashboard's own background and a theme's wallpaper are not drawn at all.
	dash.hide_aura = 0;
	if (aura_image && settings.auto_place == 3) {
		log_value("behind everything; the Aura image to hide is", aura_image);
		dash.hide_aura = 1;
	} else if (aura_image) {
		log_value("backgroundScene", background);
		log_value("its Aura image", aura_image);
		dash.parent = background;
		dash.after = aura_image;
	} else if (want_aura) {
		const HXUIOBJ aura = xui_descendant(root, "AuraScene");
		const HXUIOBJ theme = aura ? xui_descendant(aura, "Theme") : 0;
		const HXUIOBJ theme_parent = theme ? xui_relative(FN_GET_PARENT, theme) : 0;
		log_value("AuraScene", aura);
		log_value("its Theme group", theme);
		if (theme_parent) {
			dash.parent = theme_parent;
			dash.after = theme;
		}
	}
	return SETUP_NEXT;
}

// Step 2: the video element, not yet in the tree.
static int setup_create(void)
{
	static const uint16_t VIDEO_CLASS[] = { 'X', 'u', 'i', 'V', 'i', 'd', 'e', 'o', 0 };
	HXUIOBJ video = 0;
	int32_t status = xui_create_object(VIDEO_CLASS, &video);
	log_value("step 2: create XuiVideo ->", (uint32_t)status);
	if (status < 0 || !video)
		return SETUP_DONE;
	dash.video = video;
	dash.video_class = FUNCTION(FN_GET_OBJECT_CLASS, uint32_t (*)(HXUIOBJ))(video);
	log_value("its class handle", dash.video_class);

	status = FUNCTION(FN_SET_BOUNDS, int32_t (*)(HXUIOBJ, float, float))(video, (float)settings.width,
		(float)settings.height);
	log_value("set bounds ->", (uint32_t)status);
	log_value("SizeMode ->", (uint32_t)xui_set_property(video, "SizeMode", PROP_UNSIGNED, SIZE_MODE_STRETCH));
	// The element's own Loop is only used for one file that is loaded whole, the case seen looping for hours.
	// A streamed file is never looped by the player: the user's console crashed when big (streamed) videos
	// reached their end with Loop on (2026-11-13). Those, and playlists, end and stop; follow_playlist then
	// starts the next file - or the same one again - on the stopped player.
	// A file whose length couldn't be read can't be left before its end, so it loops in the player as well and
	// the playlist stays on it: a repeating video rather than a crash.
	if (dash.current >= settings.video_count)
		dash.current = 0;
	const bool will_stream = settings.stream && player_hooked && !stream_failed && placeholder_bytes;
	// One video always loops inside the player, streamed or not: that is what 0.13-0.18 did on the user's
	// console without a reported crash, where the leave-early-and-rebuild used for playlists has crashed the
	// friend's console in every version so far. 0.19 and 0.20 used the rebuild for one streamed video too.
	(void)will_stream;
	const uint32_t loop = 1;
	dash.loop_on = loop;
	log_value(loop ? "Loop on ->" : "Loop off ->", (uint32_t)xui_set_property(video, "Loop", PROP_BOOL, loop));
	if (settings.volume_set) {
		union {
			float number;
			uint32_t bits;
		} volume = { .number = (float)settings.volume_db };
		log_value("Volume ->", (uint32_t)xui_set_property(video, "Volume", PROP_FLOAT, volume.bits));
	}
	return SETUP_NEXT;
}

// Step 3: into the tree.
static int setup_insert(void)
{
	const HXUIOBJ video = dash.video;
	int32_t status;
	log_value("step 3: insert under", dash.parent);
	if (dash.after) {
		// Previous and next sibling given: the child goes between them; no next sibling appends
		// (decomp/insert.c).
		status = FUNCTION(FN_INSERT_CHILD, int32_t (*)(HXUIOBJ, HXUIOBJ, HXUIOBJ, HXUIOBJ))(dash.parent, video,
			dash.after, xui_relative(FN_GET_NEXT, dash.after));
		log_value("insert after that element ->", (uint32_t)status);
		log_value("its next sibling is now", xui_relative(FN_GET_NEXT, dash.after));
	} else {
		status = settings.front ? xui_add_child(dash.parent, video) : xui_insert_first(dash.parent, video);
		log_value(settings.front ? "add as last child ->" : "insert as first child ->", (uint32_t)status);
	}
	if (status < 0) {
		xui_destroy_object(video);
		dash.video = 0;
		return SETUP_DONE;
	}
	log_value("video element", video);

	for (uint32_t i = 0; i < settings.hide_count; i++) {
		const HXUIOBJ hidden = xui_descendant(dash.root, settings.hide[i]);
		if (hidden)
			FUNCTION(FN_SET_OPACITY, int32_t (*)(HXUIOBJ, float))(hidden, 0.0f);
		log_text_value(hidden ? "hidden:" : "hide element not found:", settings.hide[i]);
	}
	return SETUP_NEXT;
}

static int setup_step(void)
{
	switch (dash.step) {
	case STEP_FIND:
		return setup_find();
	case STEP_CREATE:
		return setup_create();
	case STEP_INSERT:
		return setup_insert();
	default:
		log_line("step 4: first file");
		play(dash.current < settings.video_count ? dash.current : 0);
		dash.created = 1;
		if (signin_state)
			dash.signin = signin_mask();
		return SETUP_DONE;
	}
}

static void rebuild_video(bool slow);

// Goes to the next video (or the same one again) before the current one ends.
//
// Three crashes changing videos, all on the friend's console, all with the old player at or near the end of
// its file: 0.6 set the next File the moment the looping player began its restart; 0.17 waited for the player
// to stop, set the next File, and died right after the new player was asked for; 0.19 destroyed the element
// 2 s before the end and died within half a second (step 1 of the new setup is the log's last entry).
// What has never crashed, in any log, is destroying the element while its video is in the middle of its file:
// every sign-in does it. A streamed player reads about 1 MB ahead of what it shows, so 2 s before the end of
// what is shown its reading had already hit the end of the file; "at the end" is the suspect, not proven.
// So: the file is left earlier, by a margin worked out from its size (video_length_ms), while the player is
// still reading; then nothing is made for a second, and the new element's steps run half a second apart, so
// the old player is gone before a new one is asked for and the log is on the stick between steps.
static void follow_playlist(void)
{
	if (!dash.video)
		return;
	uint32_t *instance = NULL;
	const int32_t status = FUNCTION(FN_OBJECT_FROM_HANDLE, int32_t (*)(HXUIOBJ, uint32_t **))(dash.video, &instance);
	// The instance's first word is its own handle (decomp/video2.c, FUN_927ce130); anything else and the
	// layout isn't the one read here.
	if (status < 0 || !instance || !readable(instance, (VIDEO_BUFFER_WORD + 1) * 4) || instance[0] != dash.video)
		return;
	// The word flips between 0 and 1 with every frame the player hands over, and is -1 before a file's first
	// frame (decomp/video3.c, FUN_927ccfe8).
	const uint32_t buffer = instance[VIDEO_BUFFER_WORD];
	if (buffer != dash.last_buffer) {
		dash.last_buffer = buffer;
		dash.buffer_frame = dash.frames;
		if (buffer != 0xFFFFFFFF && !dash.moving) {
			dash.moving = 1;
			KeQuerySystemTime(&dash.first_frame_time);
		}
	}
	const uint32_t still = dash.frames - dash.buffer_frame;
	if (!dash.moving) {
		if (dash.streaming && still >= 480) {
			// Eight seconds and no first frame from a streamed file: streaming doesn't work on this dashboard
			// (or for this file). Stop using it and load files whole, the way that is known to work.
			log_text_value("no picture when streamed; loading files whole from now on:", settings.videos[dash.current]);
			stream_failed = 1;
			rebuild_video(true);
		} else if (still >= 1200 && settings.video_count > 1) {
			// Twenty seconds without a first frame: a file the player can't open. Go on rather than stop here.
			log_text_value("no picture from this file, skipping it:", settings.videos[dash.current]);
			dash.current = (dash.current + 1) % settings.video_count;
			rebuild_video(true);
		}
		return;
	}
	// With Loop on the player restarts the file itself and the plugin leaves it alone.
	if (dash.loop_on || !settings.video_count)
		return;
	uint64_t now = 0;
	KeQuerySystemTime(&now);
	const uint32_t played_ms = (uint32_t)((now - dash.first_frame_time) / 10000);
	const uint32_t length_ms = settings.lengths_ms[dash.current];
	if (length_ms && played_ms + settings.margins_ms[dash.current] >= length_ms) {
		log_take();
		log_raw("near the end of ");
		log_raw(settings.videos[dash.current]);
		log_raw(" after ms ");
		log_number_raw(played_ms);
		log_raw(" of ");
		log_number_raw(length_ms);
		log_raw("; removing it\r\n");
		log_give();
		dash.current = (dash.current + 1) % settings.video_count;
		rebuild_video(true);
		log_text_value("removed; next after a pause:", settings.videos[dash.current]);
	}
	// If the file ended first all the same (it stands still), it is left showing its last frame: letting go
	// of a player that has reached its end is what crashed.
}

// Whether the handle still names the XuiVideo this plugin made and it still has a parent. A destroyed
// element's handle stops resolving (handles carry a generation in their high half, as the tree dump shows), and
// if the number is ever reused the class or the instance's own-handle word won't match.
static bool video_alive(void)
{
	if (!dash.video)
		return false;
	if (FUNCTION(FN_GET_OBJECT_CLASS, uint32_t (*)(HXUIOBJ))(dash.video) != dash.video_class)
		return false;
	uint32_t *instance = NULL;
	const int32_t status = FUNCTION(FN_OBJECT_FROM_HANDLE, int32_t (*)(HXUIOBJ, uint32_t **))(dash.video, &instance);
	if (status < 0 || !instance || !readable(instance, 4) || instance[0] != dash.video)
		return false;
	return xui_relative(FN_GET_PARENT, dash.video) != 0;
}

// Writes what can be read of the video element to the log, and the element tree when dump=1.
static void report_video(const char *when)
{
	log_text_value("-- video state", when);
	log_value("alive", video_alive());
	if (dash.video) {
		log_value("parent", xui_relative(FN_GET_PARENT, dash.video));
		log_value("previous sibling wanted", dash.after);
		if (dash.after)
			log_value("its next sibling", xui_relative(FN_GET_NEXT, dash.after));
		uint32_t *instance = NULL;
		const int32_t status = FUNCTION(FN_OBJECT_FROM_HANDLE, int32_t (*)(HXUIOBJ, uint32_t **))(dash.video, &instance);
		if (status >= 0 && instance && readable(instance, 0x6C) && instance[0] == dash.video) {
			// Words of XuiVideo's instance (NOTES.md): shader, current buffer, width, height, player.
			log_value("shader", instance[0xB]);
			log_value("current buffer", instance[0xD]);
			log_value("video width", instance[0xE]);
			log_value("video height", instance[0xF]);
			log_value("player", instance[0x14]);
		}
	}
	const HXUIOBJ focus = FUNCTION(FN_GET_FOCUS, HXUIOBJ (*)(void))();
	HXUIOBJ root = focus;
	for (int depth = 0; root && depth < 64; depth++) {
		const HXUIOBJ parent = xui_relative(FN_GET_PARENT, root);
		if (!parent)
			break;
		root = parent;
	}
	log_value("root now", root);
	log_value("root at setup", dash.root);
	if (root) {
		const HXUIOBJ aura = xui_descendant(root, "AuraScene");
		log_value("AuraScene now", aura);
		log_value("its Theme group now", aura ? xui_descendant(aura, "Theme") : 0);
		if (settings.dump) {
			log_line("-- element tree (handle, class, id) --");
			dump_nodes = 0;
			dump_tree(root, 0);
			log_value("-- end of tree, elements listed:", dump_nodes);
		}
	}
}

// Destroys the video element if it is there and starts setup again from the first step, with dash.current
// as the file to play. Quick (after a sign-in or a changed folder, where the dashboard already took the old
// one or the same file comes back): the steps run two frames apart. Slow (going to another video): a second
// with nothing, then the steps half a second apart.
static void rebuild_video(bool slow)
{
	if (video_alive()) {
		FUNCTION(FN_UNLINK, int32_t (*)(HXUIOBJ))(dash.video);
		xui_destroy_object(dash.video);
	}
	dash.video = 0;
	dash.created = 0;
	dash.attempts = 0;
	dash.step = STEP_FIND;
	dash.step_frame = dash.frames;
	dash.step_gap = slow ? 60 : 1;
	dash.rebuilding = !slow;
	dash.stage = STAGE_WAITING;
}

// Called by the hook once per presented dashboard frame.
void wmvslop_tick(void)
{
	if (!dash.armed)
		return;
	dash.frames++;
	if (scan_pending) {
		// The folder's videos changed. Take the new list and start over with it: the element is made again
		// because whether it loops depends on how many files there are.
		take_listing();
		scan_pending = 0;
		log_listing();
		if (dash.hide_aura && dash.root) {
			dash.hidden_aura = 0;
			keep_aura_hidden(1.0f);
		}
		dash.current = 0;
		rebuild_video(false);
		return;
	}
	if (dash.stage == STAGE_WAITING) {
		if (dash.frames < settings.delay_frames || dash.frames - dash.step_frame < dash.step_gap)
			return;
		dash.step_frame = dash.frames;
		const int result = setup_step();
		if (result == SETUP_NEXT) {
			dash.step++;
			// The first setup after the hook goes in leaves half a second between steps, so each step's log
			// lines are on the stick before the next runs. Putting the video back later (after a sign-in) has
			// run those same steps before and goes as fast as it can.
			dash.step_gap = dash.rebuilding ? 2 : STEP_FRAMES_APART;
		} else if (result == SETUP_DONE) {
			dash.stage = dash.created ? STAGE_PLAYING : STAGE_FAILED;
		} else {
			// Nothing to work with yet: look again in a quarter of a second, for up to 30 s.
			dash.step_gap = RETRY_FRAMES_APART;
			if (++dash.attempts == 120) {
				log_line("no focused element after 30 s; giving up");
				dash.stage = STAGE_FAILED;
			}
		}
	} else if (dash.stage == STAGE_PLAYING) {
		// The dashboard rebuilds its background at times (the user saw the video vanish after signing in to
		// Xbox Live), and the video element goes with the old one. Twice a second, check it is still there
		// and still in the tree; if not, set up again from the first step.
		if (dash.frames % 30 == 0 && signin_state) {
			// Someone signing in or out: the user's console loses the picture then, without the element being
			// destroyed as far as 0.5's check could tell. Five seconds later, write down what state the video
			// is in and build it again from nothing.
			const uint32_t mask = signin_mask();
			if (mask != dash.signin) {
				log_value("sign-in state changed to", mask);
				dash.signin = mask;
				dash.rebuild_frame = dash.frames + 300;
			}
		}
		if (dash.rebuild_frame && dash.frames >= dash.rebuild_frame) {
			dash.rebuild_frame = 0;
			// Only a note for the log. The user's 0.6 log shows what sign-in does: the dashboard builds a new
			// AuraScene (handle 000101BE became 000201D9) and the check below had already put a new video in
			// it by this point. 0.6 also destroyed and rebuilt it here, which restarted the clip a second time.
			report_video("5 s after the sign-in change");
		}
		if (dash.hide_aura && dash.frames % 10 == 5)
			keep_aura_hidden(0.0f);
		if (dash.frames % 10 == 0 && !video_alive()) {
			// Nothing to show behind it now: let the dashboard's own background be seen until the video is back.
			if (dash.hide_aura) {
				dash.hidden_aura = 0;
				keep_aura_hidden(1.0f);
			}
			log_value("no video element in the tree; setting up again, frame", dash.frames);
			rebuild_video(false);
			return;
		}
		follow_playlist();
	}
}

// ---------------------------------------------------------------------------------------------------------
// Resolver's memory access.

uint32_t resolve_read32(uint32_t address)
{
	return *(const volatile uint32_t *)(uintptr_t)address;
}

uint32_t resolve_valid(uint32_t address)
{
	return MmIsAddressValid((const void *)(uintptr_t)address);
}

static HANDLE xam_module;

uint32_t resolve_xam_export(uint32_t ordinal)
{
	void *address = NULL;
	if (!NT_SUCCESS(XexGetProcedureAddress(xam_module, ordinal, &address)))
		return 0;
	return (uint32_t)(uintptr_t)address;
}

// ---------------------------------------------------------------------------------------------------------
// The system thread.

// The dashboard's thunk for an import: lis r11 ; ori r11, r11 or addi r11, r11 ; mtctr r11 ; bctr.
static uint32_t *find_thunk(const ResolveImage *image, uint32_t target, bool *add_form)
{
	for (uint32_t s = 0; s < image->section_count; s++) {
		const uint32_t *section = (const uint32_t *)(uintptr_t)(image->sections + s * 40);
		// PE fields are little-endian.
		const uint32_t flags = __builtin_bswap32(section[9]);
		if (!(flags & 0x20000000))
			continue;
		const uint32_t size = __builtin_bswap32(section[2]);
		uint32_t *words = (uint32_t *)(uintptr_t)(image->base + __builtin_bswap32(section[3]));
		for (uint32_t i = 0; i + 4 <= size / 4; i++) {
			if (words[i + 2] != 0x7D6903A6 || words[i + 3] != 0x4E800420 || (words[i] & 0xFFFF0000) != 0x3D600000)
				continue;
			const uint32_t high = words[i] & 0xFFFF;
			const uint32_t low = words[i + 1] & 0xFFFF;
			if ((words[i + 1] & 0xFFFF0000) == 0x616B0000 && (high << 16 | low) == target) {
				*add_form = false;
				return &words[i];
			}
			if ((words[i + 1] & 0xFFFF0000) == 0x396B0000 && (high << 16) + (uint32_t)(int16_t)low == target) {
				*add_form = true;
				return &words[i];
			}
		}
	}
	return NULL;
}

// The system's own apps (the avatar editor and others) run under the dashboard's title ID at the dashboard's
// address, so the title ID alone doesn't say which program is loaded. The identity is the image's PE time
// stamp and size: when it changes, a different program is there and the hook went with the old one.
static uint32_t image_identity;

// XamGetCurrentTitleId, looked up once xam.xex is loaded. The plugin imports nothing from xam: DashLaunch
// loads plugins at power-on, and a .xex that imports from a module not loaded yet can't be loaded then.
static uint32_t (*current_title_id)(void);
enum { XAM_GET_CURRENT_TITLE_ID_ORDINAL = 0x1CF, XAM_USER_GET_SIGNIN_STATE_ORDINAL = 0x210 };

static bool dashboard_running(uint32_t *identity)
{
	if (current_title_id() != DASH_TITLE_ID)
		return false;
	ResolveImage image;
	if (resolve_open_image(DASH_BASE, &image))
		return false;
	const uint32_t pe = DASH_BASE + __builtin_bswap32(resolve_read32(DASH_BASE + 0x3C));
	*identity = resolve_read32(pe + 8) ^ image.size;
	return true;
}

static uint32_t *installed_thunk;
static uint32_t installed_word;

// False once the hooked thunk no longer holds the plugin's word (the image was replaced).
static bool hook_in_place(void)
{
	return installed_thunk && MmIsAddressValid(installed_thunk) && *(volatile uint32_t *)installed_thunk == installed_word;
}

static void flush_log(void)
{
	static char path[PATH_CHARS];
	if (!path[0])
		folder_path(path, sizeof(path), "WMVSlop.log");
	// The bare lock: log_take would add a time stamp, which is itself new text. 0.14 did that here, so every
	// half second the log "changed" and the whole file was truncated and rewritten; turning the console off
	// then left it empty (the user's 0.14 log was 0 bytes).
	log_lock_only();
	const uint32_t used = log_used;
	log_give();
	if (used == log_written)
		return;
	// The first write of a start replaces the old log. Later ones add the new text at its place, and never
	// truncate: power can go at any moment. If the file had to be made anew (stick out and back in with the
	// log deleted), everything is written again from the start, or the gap would show old disk contents.
	HANDLE handle;
	IO_STATUS_BLOCK opened = { 0 };
	const ANSI_STRING name = { .length = (uint16_t)text_length(path),
		.maximum_length = (uint16_t)(text_length(path) + 1), .pointer = path };
	const OBJECT_ATTRIBUTES attributes = { .root_directory = 0, .name = &name, .attributes = OBJ_CASE_INSENSITIVE };
	if (!NT_SUCCESS(NtCreateFile(&handle, GENERIC_WRITE | SYNCHRONIZE, &attributes, &opened, NULL,
			FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
			log_written == 0 ? FILE_OVERWRITE_IF : FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)))
		return;
	enum { FILE_WAS_CREATED = 2 };
	const uint32_t from = opened.information == FILE_WAS_CREATED ? 0 : log_written;
	IO_STATUS_BLOCK written = { 0 };
	const uint64_t offset = from;
	const NTSTATUS status = NtWriteFile(handle, 0, NULL, NULL, &written, log_text + from, used - from, &offset);
	NtClose(handle);
	if (NT_SUCCESS(status))
		log_written = used;
}

// There is no guard file any more (0.1-0.3 wrote WMVSlop.guard and turned the plugin off when a start looked
// unfinished; the user had it removed on 2026-11-12 because it kept the plugin from starting). What is kept is
// the last start's log, copied to WMVSlop-previous.log, so a start that froze can still be read afterwards.
static void keep_previous_log(void)
{
	char path[PATH_CHARS];
	folder_path(path, sizeof(path), "WMVSlop.log");
	static char previous[LOG_BYTES];
	const int32_t bytes = file_read_all(path, previous, sizeof(previous));
	if (bytes <= 0)
		return;
	folder_path(path, sizeof(path), "WMVSlop-previous.log");
	file_write_at(path, previous, (uint32_t)bytes, 0, true);
}

// Redirects the one call in the dashboard's player wrapper to wmvslop_player_hook, so every video player the
// dashboard makes gets its threads spread over the cores. One aligned word is rewritten, as with the thunk.
// Nothing happens unless exactly one dashboard function is that wrapper, instruction for instruction.
static void hook_player(void)
{
	player_hooked = 0;
	stream_failed = 0;
	const uint32_t wrapper = resolve_find_pattern(player_wrapper_words, player_wrapper_masks, PLAYER_WRAPPER_WORDS);
	log_value("player wrapper", wrapper);
	if (!wrapper) {
		log_line("this dashboard's player wrapper isn't the known one: no streaming, threads stay where they are");
		return;
	}
	uint32_t *call = (uint32_t *)(uintptr_t)(wrapper + PLAYER_WRAPPER_CALL * 4);
	const uint32_t word = *(volatile uint32_t *)call;
	// A relative bl: displacement in bits 2-25, signed.
	int32_t displacement = (int32_t)(word & 0x03FFFFFC);
	if (displacement & 0x02000000)
		displacement -= 0x04000000;
	const uint32_t target = (uint32_t)(uintptr_t)call + (uint32_t)displacement;
	if (target == (uint32_t)(uintptr_t)wmvslop_player_hook) {
		// Already redirected (same dashboard image looked at twice); the creator's address is still known.
		player_hooked = wmvslop_real_create_player != 0;
		return;
	}
	wmvslop_real_create_player = target;
	const int32_t to_hook = (int32_t)((uint32_t)(uintptr_t)wmvslop_player_hook - (uint32_t)(uintptr_t)call);
	if (to_hook >= 0x02000000 || to_hook < -0x02000000) {
		log_line("the plugin is loaded too far from the dashboard for a direct call; threads stay as they are");
		return;
	}
	log_value("player creator", wmvslop_real_create_player);
	flush_log();
	*(volatile uint32_t *)call = 0x48000001 | ((uint32_t)to_hook & 0x03FFFFFC);
	wmvslop_flush_code(call);
	player_hooked = 1;
	log_value("player requests now come through the plugin; the call is", *(volatile uint32_t *)call);
}

// The full search runs on every start (about two seconds). 0.17 kept its result in a WMVSlop.found file to skip
// it; the two starts that had that file to read are the two where the user saw no video and a crash, and the
// user asked for the plugin not to need the file. resolve_from_saved is still in resolve.c, unused.
static bool install(void)
{
	flush_log();
	const int found = resolve_all(DASH_BASE);
	log_take();
	log_raw("functions found: ");
	log_number_raw(found < 0 ? 0 : (uint32_t)found);
	log_raw(" of ");
	log_number_raw(FN_COUNT);
	log_raw(", anchors ");
	log_number_raw((uint32_t)resolve_anchor_count());
	log_raw("\r\n");
	// The list only when it is complete: a failed try is repeated, and one line each is enough.
	for (int i = 0; i < FN_COUNT && found == FN_COUNT; i++) {
		log_raw("  ");
		log_raw(resolve_functions[i].name);
		log_raw(" ");
		log_hex_raw(resolve_functions[i].dash);
		log_raw(" how ");
		log_number_raw(resolve_functions[i].how);
		log_raw("\r\n");
	}
	log_give();
	if (found != FN_COUNT) {
		log_value("not every function was found; the dashboard is left alone for now. result", (uint32_t)found);
		return false;
	}

	HANDLE kernel = 0;
	void *vdswap = NULL;
	if (XexGetModuleHandle("xboxkrnl.exe", &kernel) != 0
		|| !NT_SUCCESS(XexGetProcedureAddress(kernel, VDSWAP_ORDINAL, &vdswap)) || !vdswap) {
		log_line("VdSwap not found in the kernel");
		return false;
	}
	ResolveImage image;
	if (resolve_open_image(DASH_BASE, &image))
		return false;
	bool add_form = false;
	uint32_t *thunk = find_thunk(&image, (uint32_t)(uintptr_t)vdswap, &add_form);
	log_value("VdSwap", (uint32_t)(uintptr_t)vdswap);
	log_value("dashboard's VdSwap thunk", (uint32_t)(uintptr_t)thunk);
	if (!thunk) {
		log_line("the dashboard has no VdSwap thunk of the expected shape; left alone");
		return false;
	}

	if (settings.spread || settings.stream)
		hook_player();

	memset(&dash, 0, sizeof(dash));
	wmvslop_real_vdswap = (uint32_t)(uintptr_t)vdswap;
	const uint32_t low = thunk[1] & 0xFFFF;
	// An addi thunk adds a signed low half, so it needs the high half one up when the low half is negative.
	const uint32_t high = ((uint32_t)(uintptr_t)wmvslop_sled >> 16) + (add_form && (low & 0x8000) ? 1 : 0);
	// For the log only: whether the kernel reports the thunk's page as writable isn't known yet.
	log_value("thunk page protection", MmQueryAddressProtect((uint32_t)(uintptr_t)thunk));
	flush_log();
	dash.armed = 1;
	installed_thunk = thunk;
	installed_word = 0x3D600000 | (high & 0xFFFF);
	*(volatile uint32_t *)thunk = installed_word;
	wmvslop_flush_code(thunk);
	log_value("hook installed, thunk word 0 is now", thunk[0]);
	flush_log();
	return true;
}

static void plugin_thread(void *context)
{
	(void)context;
	log_line("WMVSlop 0.22");

	// Storage isn't mounted when plugins load.
	while (!find_folder())
		sleep_ms(2000);
	keep_previous_log();

	log_text_value("folder", settings.folder);
	read_settings();
	list_videos(&scan);
	measure_videos(&scan);
	take_listing();
	log_listing();

	// xam.xex is loaded after the plugins at power-on.
	for (;;) {
		void *address = NULL;
		if (XexGetModuleHandle("xam.xex", &xam_module) == 0
			&& NT_SUCCESS(XexGetProcedureAddress(xam_module, XAM_GET_CURRENT_TITLE_ID_ORDINAL, &address)) && address) {
			current_title_id = (uint32_t (*)(void))address;
			break;
		}
		sleep_ms(1000);
	}
	log_line("xam.xex found");
	void *signin = NULL;
	if (NT_SUCCESS(XexGetProcedureAddress(xam_module, XAM_USER_GET_SIGNIN_STATE_ORDINAL, &signin)) && signin)
		signin_state = (uint32_t (*)(uint32_t))signin;

	enum { IDLE, HOOKED, GAVE_UP } state = IDLE;
	uint32_t seen_ms = 0;
	uint32_t retries = 0;
	uint32_t retry_ms = 0;
	uint32_t scan_ms = 0;
	for (;;) {
		sleep_ms(500);
		// Every four seconds, look at the folder again: videos can be added, removed or swapped (stick out
		// and back in) without restarting the console. A changed listing is handed to the dashboard's thread.
		scan_ms += 500;
		if (scan_ms >= 4000 && !scan_pending) {
			scan_ms = 0;
			if (list_videos(&scan) && scan.signature != live_signature) {
				measure_videos(&scan);
				log_value("the folder's videos changed; now", scan.video_count);
				scan_pending = 1;
			}
		}
		uint32_t identity = 0;
		bool running = dashboard_running(&identity);
		if (running && state != IDLE && (identity != image_identity || (state == HOOKED && !hook_in_place()))) {
			// Another program took the dashboard's place (or the dashboard came back): start over with it.
			log_line("the program at the dashboard's address changed");
			running = false;
		}
		if (!running) {
			// The dashboard's image went with it, hook included.
			if (state != IDLE) {
				dash.armed = 0;
				log_line("dashboard closed");
				installed_thunk = NULL;
			}
			state = IDLE;
			seen_ms = 0;
			flush_log();
			continue;
		}
		if (state == IDLE) {
			// Let it finish loading before reading its code.
			seen_ms += 500;
			// Two seconds, as in 0.15, the last version with no reported trouble (0.16/0.17 waited one).
			if (seen_ms >= 2000) {
				log_value("dashboard running, image identity", identity);
				image_identity = identity;
				state = install() ? HOOKED : GAVE_UP;
				retries = 0;
				retry_ms = 0;
			}
		} else if (state == GAVE_UP && retries < 40) {
			// At power-on the dashboard's image is at its address before all of it has been read in: the
			// user's console found 0 of 19 functions (1 anchor) on its first look, and the same dashboard
			// gives 19 of 19 once loaded. So a failed look is repeated, every 3 s for two minutes.
			retry_ms += 500;
			if (retry_ms >= 3000) {
				retry_ms = 0;
				retries++;
				log_value("looking at the dashboard again, try", retries);
				if (install())
					state = HOOKED;
			}
		}
		flush_log();
	}
}

// DllMain: the loader calls it with reason 1 when DashLaunch loads the plugin.
uint32_t wmvslop_entry(HANDLE module, uint32_t reason, void *reserved)
{
	(void)module;
	(void)reserved;
	// One thread only, however often the plugin is loaded: a second one (DashLaunch's setting plus a load by
	// hand, as in the user's 0.5 log) shares every variable with the first and garbles the log.
	static volatile uint32_t started;
	if (reason == 1 && !__sync_lock_test_and_set(&started, 1)) {
		HANDLE thread = 0;
		uint32_t thread_id = 0;
		// Flag 2 makes it a system thread, which outlives titles.
		ExCreateThread(&thread, 0x10000, &thread_id, NULL, plugin_thread, NULL, 2);
	}
	return 1;
}
