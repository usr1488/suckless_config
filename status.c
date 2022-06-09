#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>

#define MAX_SYMBOLS 64
#define MEMINFO_PATH "/proc/meminfo"
#define BATTERY_CAPACITY_PATH "/sys/class/power_supply/BAT0/capacity"
#define BATTERY_STATUS_PATH "/sys/class/power_supply/BAT0/status"
#define MILLIS_TO_NANOS(M) (M * 1000000 > 999999999 ? 999999999 : M * 1000000) // nanosleep range check
#define CLOCKT_TO_NANOS(C) (MILLIS_TO_NANOS((C / CLOCKS_PER_SEC) * 1000))

static char* ram_status(void);
static char* battery_status(void);
static char* layout_status(void);
static char* date_status(void);

char* (*status_providers[])(void) = {
	ram_status,
	battery_status,
	layout_status,
	date_status
};

Display* display;
Window window;
XkbDescRec* keyboard;

struct {
	int fd;               // file descriptor for /proc/meminfo
	char* mem_total;      // null terminated string with numbers only from first line in /proc/meminfo
	char* mem_total_fmt;  // formatted 'mem_total' - '7.67 GiB', '526 MiB' etc.
	char* buffer;         // allocated once in setup(), used for subsequent reads
	unsigned char size;   // length of single line in /proc/meminfo, used for allocating 'buffer'
} ram;

struct {
	int capacity_fd;
	int status_fd;
	char buffer[10];
} battery;

struct {
	unsigned char changed; // set by dwm
	char* buffer;
} layout;

void signal_handler(int, siginfo_t* info, void*) {
	layout.changed = info -> si_value.sival_int;
}

// UTILS BEGIN

static void die(char* msg) {
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(EXIT_FAILURE);
}

/* remove 'target' string from 'str' string
 * doesn't work with string literals passed to 'str', fails with segfault due to modification of .rodata segment
 */
char* str_cut(char* str, char* target) {
	int str_length = strlen(str);
	int target_length = strlen(target);

	if (target_length > str_length) {
		return str;
	}

	for (int a = 0; a < str_length; a++) {
		if (str[a] == *target) {
			for (int j = 1; j < target_length; j++) {
				if (str[a + j] != target[j]) {
					a += j - 1; // skip checked bytes
					goto not_found;
				}
			}

			// dest - index of first 'target' character
			// src  - index of first character after 'target'
			// n    - length of string after 'target', plus terminating byte
			memcpy(str + a, str + a + target_length, str_length - target_length - a + 1);
			return str;

			not_found:
		}
	}

	return str;
}

/* remove whitespaces at the begining and at the end of 'str'
 * doesn't work with string literals, fails with segfault due to modification of .rodata segment
 */
char* str_trim(char* str) {
	int str_length = strlen(str);

	if (*str != ' ' && str[str_length - 1] != ' ') {
		return str;
	}

	for (int a = 0; a < str_length; a++) {
		if (str[a] != ' ') {
			memcpy(str, str + a, str_length - a + 1);
			str_length -= a;
			break;
		}
	}

	for (int a = str_length - 1; a >= 0; a--) {
		if (str[a] != ' ') {
			str[a + 1] = 0;
			break;
		}
	}

	return str;
}

ssize_t pread_all(int fd, void* buffer, size_t amount, off_t offset) {
	ssize_t total = 0, b;

	while ((b = pread(fd, buffer + total, amount - total, offset + total))) {
		if (b == -1) {
			return b;
		} else if ((total += b) == amount) {
			return total;
		}
	}

	return total;
}

void format_kb(int kb, char* buffer, int size) {
	if (kb >= 1024 * 1024) {
		snprintf(buffer, size, "%.2f GiB", kb / (float) (1024 * 1024));
	} else if (kb >= 1024) {
		snprintf(buffer, size, "%d MiB", kb / 1024);
	} else {
		snprintf(buffer, size, "%d KiB", kb);
	}
}

// UTILS END

// STATUS_PROVIDERS START

char* ram_status(void) {
	int mem_free, buffers, cached, sreclaimable;
	char mem_used[ram.size];

	fsync(ram.fd);

	// MemFree
	if (pread_all(ram.fd, ram.buffer, ram.size, ram.size) == -1) {
		perror("ram_status mem_free read error");
		return "";
	}

	ram.buffer[ram.size - 1] = 0; // replace \n
	str_cut(ram.buffer, "MemFree:");
	str_cut(ram.buffer, "kB");
	str_trim(ram.buffer);
	mem_free = atoi(ram.buffer);

	// Buffers
	if (pread_all(ram.fd, ram.buffer, ram.size, ram.size * 3) == -1) {
		perror("ram_status buffers read error");
		return "";
	}

	ram.buffer[ram.size - 1] = 0; // replace \n
	str_cut(ram.buffer, "Buffers:");
	str_cut(ram.buffer, "kB");
	str_trim(ram.buffer);
	buffers = atoi(ram.buffer);

	// Cached
	if (pread_all(ram.fd, ram.buffer, ram.size, ram.size * 4) == -1) {
		perror("ram_status cached read error");
		return "";
	}

	ram.buffer[ram.size - 1] = 0; // replace \n
	str_cut(ram.buffer, "Cached:");
	str_cut(ram.buffer, "kB");
	str_trim(ram.buffer);
	cached = atoi(ram.buffer);

	// SReclaimable
	if (pread_all(ram.fd, ram.buffer, ram.size, ram.size * 23) == -1) {
		perror("ram_status sreclaimable read error");
		return "";
	}

	ram.buffer[ram.size - 1] = 0; // replace \n
	str_cut(ram.buffer, "SReclaimable:");
	str_cut(ram.buffer, "kB");
	str_trim(ram.buffer);
	sreclaimable = atoi(ram.buffer);

	format_kb(
			(atoi(ram.mem_total) - mem_free) - (buffers + cached + sreclaimable), // used memory
			mem_used, 
			ram.size
	);
	snprintf(ram.buffer, ram.size, "%s/%s", mem_used, ram.mem_total_fmt);

	return ram.buffer; 
}

char* battery_status(void) {
	if (battery.capacity_fd == -1) {
		return "";
	}

	int charging, b;
	char capacity[5] = { 0 };

	memset(battery.buffer, 0, sizeof(battery.buffer));

	// use pread(2) instead of read(2), to avoid calling lseek(2)
	if (pread_all(battery.status_fd, battery.buffer, sizeof(battery.buffer), 0) == -1) {
		perror("battery_status read "BATTERY_STATUS_PATH" error");
		return "";
	}

	charging = !strcmp(battery.buffer, "Charging\n");

	if ((b = pread_all(battery.capacity_fd, capacity, sizeof(capacity), 0)) == -1) {
		perror("battery_status read "BATTERY_CAPACITY_PATH" error");
		return "";
	}

	capacity[b - 1] = 0; // replace \n
	snprintf(battery.buffer, sizeof(battery.buffer), "%c%s %%", charging ? '+' : ' ', capacity);

	return battery.buffer;
}

char* layout_status(void) {
	char* symbols;

	if (layout.changed) {
		layout.changed = 0;

		XkbGetNames(display, XkbSymbolsNameMask, keyboard);
		symbols = XGetAtomName(display, keyboard -> names -> symbols);
		strcpy(layout.buffer, symbols);
		XFree(symbols);
		XkbFreeNames(keyboard, XkbSymbolsNameMask, False);

		strcpy(layout.buffer, strchr(layout.buffer, '+') + 1);
		layout.buffer[2] = 0; // xkb symbols contains only 2 chars - us, cz, de etc.
	}

	return layout.buffer;
}

char* date_status(void) {
	static char buffer[28];
	
	strftime(
			buffer, 
			sizeof(buffer), 
			"%d/%m/%Y %H:%M:%S %a %b", 
			localtime(&(time_t) { time(NULL) })
	);

	return buffer;
}

// STATUS_PROVIDERS END

// SETUP START

void ram_setup(void) {
	int read_size = 32, amount;
	char* tmp;

	if ((ram.fd = open(MEMINFO_PATH, O_RDONLY)) == -1) {
		die("setup_ram open "MEMINFO_PATH" error");
	}

	if (!(tmp = malloc(read_size))) {
		fputs("setup_ram tmp malloc error\n", stderr);
		exit(EXIT_FAILURE);
	}

	while (1) {
		if ((amount = pread_all(ram.fd, tmp, read_size, 0)) == -1) {
			die("setup_ram tmp read error");
		}

		for (int a = 0; a < amount; a++) {
			if (tmp[a] == '\n') { // calculate line length to avoid reading of entire file
				ram.size = a + 1;
				free(tmp);
				goto mem_total_fmt;
			}
		}

		read_size *= 2;

		if (!(tmp = realloc(tmp, read_size))) {
			fputs("setup_ram tmp realloc error\n", stderr);
			exit(EXIT_FAILURE);
		}
	}

	mem_total_fmt:
	if (!(ram.buffer = malloc(ram.size))) {
		fputs("setup_ram ram.buffer malloc error\n", stderr);
		exit(EXIT_FAILURE);
	}

	// MemTotal
	if (pread_all(ram.fd, ram.buffer, ram.size, 0) == -1) {
		die("setup_ram ram.mem_total read error");
	}

	ram.buffer[ram.size - 1] = 0; // replace \n
	str_cut(ram.buffer, "MemTotal:");
	str_cut(ram.buffer, "kB");
	str_trim(ram.buffer);

	if (!(ram.mem_total = malloc(strlen(ram.buffer) + 1))) {
		fputs("setup_ram ram.mem_total malloc error\n", stderr);
		exit(EXIT_FAILURE);
	}

	strcpy(ram.mem_total, ram.buffer);
	format_kb(atoi(ram.mem_total), ram.buffer, ram.size);

	if (!(ram.mem_total_fmt = malloc(strlen(ram.buffer) + 1))) {
		fputs("setup_ram ram.mem_total_fmt malloc error\n", stderr);
		exit(EXIT_FAILURE);
	}

	strcpy(ram.mem_total_fmt, ram.buffer);
}

void battery_setup(void) {
	battery.capacity_fd = open(BATTERY_CAPACITY_PATH, O_RDONLY);

	if (battery.capacity_fd == -1 && errno != ENOENT) {
		die("setup_battery_status open "BATTERY_CAPACITY_PATH" error");
	}

	battery.status_fd = open(BATTERY_STATUS_PATH, O_RDONLY);

	if (battery.status_fd == -1 && errno != ENOENT) {
		die("setup_battery_status open "BATTERY_STATUS_PATH" error");
	}
}

void layout_setup(void) {
	char* symbols;

	if (!(keyboard = XkbAllocKeyboard())) {
		puts("setup_keyboard_layout XkbAllocKeyboard error");
		exit(EXIT_FAILURE);
	}

	XkbGetNames(display, XkbSymbolsNameMask, keyboard);
	symbols = XGetAtomName(display, keyboard -> names -> symbols);

	if (!(layout.buffer = malloc(strlen(symbols) + 1))) {
		fputs("setup_keyboard_layout layout.buffer malloc error\n", stderr);
		exit(EXIT_FAILURE);
	}

	strcpy(layout.buffer, symbols);
	XFree(symbols);
	XkbFreeNames(keyboard, XkbSymbolsNameMask, False);

	strcpy(layout.buffer, strchr(layout.buffer, '+') + 1);
	*strchr(layout.buffer, '+') = 0;
}

void setup(void) {
	struct sigaction sig = {
		.sa_sigaction = signal_handler,
		.sa_flags = SA_SIGINFO
	};

	if (!(display = XOpenDisplay(0))) {
		fputs("setup XOpenDisplay error\n", stderr);
		exit(EXIT_FAILURE);
	}

	if (!(window = RootWindow(display, DefaultScreen(display)))) {
		fputs("setup RootWindow error\n", stderr);
		exit(EXIT_FAILURE);
	}

	if (sigaction(SIGUSR1, &sig, NULL) == -1) {
		die("setup sigaction error");
	}

	ram_setup();
	battery_setup();
	layout_setup();
}

// SETUP END

void run(void) {
	int status_text_size = MAX_SYMBOLS + 4; // plus 4 bsc of appending ' | ' in snprintf(3) and 0 byte at the end
	int current_size = 0;
	int str_length;
	long time;
	char* status_text = malloc(status_text_size); 
	char* str;
	clock_t start;
	struct timespec interval = { .tv_nsec = MILLIS_TO_NANOS(500) }, interval_rem = { 0 };
	XTextProperty xtp = {
		.encoding = XA_STRING,
		.format = 8,
		.value = (unsigned char*) status_text // suppress compiler warning
	};

	if (!status_text) {
		fputs("run malloc error\n", stderr);
		exit(EXIT_FAILURE);
	}

	while (1) {
		start = clock();

		for (int a = 0; a < sizeof(status_providers) / sizeof(*status_providers); a++) {
			str = status_providers[a]();

			str_trim(str);

			if (!(str_length = strlen(str))) {
				continue;
			}

			snprintf(status_text + current_size, status_text_size - current_size, "%s | ", str);
			current_size += str_length + 3;

			if (current_size - 3 >= MAX_SYMBOLS) {
				break;
			}
		}

		current_size -= 3;
		xtp.nitems = current_size > MAX_SYMBOLS ? MAX_SYMBOLS : current_size;
		current_size = 0;

		XSetTextProperty(display, window, &xtp, XA_WM_NAME);
		XFlush(display);

		if (nanosleep(&interval, &interval_rem) == -1 && errno == EINTR) {
			time = CLOCKT_TO_NANOS(clock() - start);
			interval.tv_nsec -= interval_rem.tv_nsec;
			interval.tv_nsec -= interval.tv_nsec - time > 0 ? time : 0;
		} else if (interval.tv_nsec != MILLIS_TO_NANOS(500)) {
			interval.tv_nsec = MILLIS_TO_NANOS(500);
		}
	}
}

void check_dwm_pid_link(void) {
	int pid, comm_fd;
	char buffer[22];
	DIR* dir = opendir("/proc");
	struct dirent* ent;

	if (!dir) {
		die("check_dwm_pid_link opendir error");
	}

	errno = 0;

	while ((ent = readdir(dir))) {
		if ((pid = atoi(ent -> d_name))) {
			snprintf(buffer, sizeof(buffer), "/proc/%d/comm", pid);

			if ((comm_fd = open(buffer, O_RDONLY)) == -1) {
				die("check_dwm_pid_link open error");
			}

			if (pread_all(comm_fd, buffer, 3, 0) == -1) {
				die("check_dwm_pid_link read error");
			}

			close(comm_fd);
			buffer[3] = 0;

			if (!strcmp(buffer, "dwm")) {
				break;
			}

			pid = 0;
		}
	}

	if (errno) {
		die("check_dwm_pid_link readdir error");
	} else if (!pid) {
		fputs("check_dwm_pid_link dwm pid not found\n", stderr);
		exit(EXIT_FAILURE);
	}

	closedir(dir);
	sigqueue(pid, SIGUSR1, (union sigval) { .sival_int = getpid() });
}

int main() {
	check_dwm_pid_link();
	setup();
	run();
	XCloseDisplay(display);
}
