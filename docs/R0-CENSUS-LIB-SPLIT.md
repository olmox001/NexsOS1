# R0.1 — Censimento simboli lib.c → mappa split (PRIMA PASSATA, euristica)

Generato 2026-07-17 da grep/awk su user/sys/lib/lib.c (157 definizioni top-level).
Da RAFFINARE a mano in R0: i casi ambigui vanno confermati; malloc.c,
font_lib, image e i portability layer sono censiti a parte in R0.2/R0.3.
Formato: DESTINAZIONE  riga  simbolo.

```
GRAPHICS	1129	graphics_draw_rect
GRAPHICS	1140	graphics_blit
GRAPHICS	1154	graphics_draw_text
GRAPHICS	1172	graphics_text_width
GRAPHICS	1197	*graphics_load_image
GRAPHICS	368	draw
GRAPHICS	371	flush
GRAPHICS	372	create_window
GRAPHICS	375	destroy_window
GRAPHICS	376	window_draw
GRAPHICS	379	window_blit
GRAPHICS	398	compositor_render
GRAPHICS	421	set_window_flags
GRAPHICS	441	*graphics_get_default_font
GRAPHICS	520	set_font
GRAPHICS	657	window_write
GRAPHICS	661	window_grid
IO	1081	input_poll_event
LIBC	1009	fseek
LIBC	1030	ftell
LIBC	1041	*strdup
LIBC	113	errno_ret
LIBC	1299	strtol
LIBC	1350	sscanf
LIBC	1373	vsscanf
LIBC	140	os1_mono_ns
LIBC	143	os1_cpu_ns
LIBC	1478	system
LIBC	1528	cmdline_split
LIBC	1573	*getenv
LIBC	1626	vfprintf
LIBC	1632	fprintf
LIBC	1642	fflush
LIBC	1652	remove
LIBC	1661	rename
LIBC	1717	utf8_decode
LIBC	1768	*strtok_r
LIBC	1789	*strtok
LIBC	1794	*strerror
LIBC	1806	strtoll
LIBC	1814	abort
LIBC	1818	qsort
LIBC	1842	file_fd
LIBC	1852	fputc
LIBC	1864	fputs
LIBC	1877	fgetc
LIBC	1896	*fgets
LIBC	1915	ungetc
LIBC	1924	clearerr
LIBC	1931	setvbuf
LIBC	1939	*tmpfile
LIBC	1950	perror
LIBC	1974	poll
LIBC	1984	signal
LIBC	1991	*mmap
LIBC	2011	ioctl
LIBC	2032	*opendir
LIBC	2044	*readdir
LIBC	2066	strspn
LIBC	2081	*strpbrk
LIBC	218	exit
LIBC	278	__win_ctl
LIBC	418	try_recv
LIBC	424	set_focus
LIBC	458	__stack_chk_fail
LIBC	588	file_write
LIBC	591	file_read
LIBC	594	list_dir
LIBC	645	vsprintf
LIBC	648	printf
LIBC	672	sprintf
LIBC	679	snprintf
LIBC	688	print_hex
LIBC	706	getchar
LIBC	717	*gets
LIBC	760	notify_send
LIBC	799	notify
LIBC	819	*fopen
LIBC	874	*freopen
LIBC	881	file_fd
LIBC	892	fstream_flush
LIBC	911	fclose
LIBC	922	fread
LIBC	968	fwrite
OS1	178	OS1low_process_spawn
OS1	183	OS1low_process_spawn_detached
OS1	187	OS1low_process_spawn_caps
OS1	197	OS1low_process_wait
OS1	208	OS1low_process_yield
OS1	210	OS1low_process_exit
OS1	220	spawn_args
OS1	225	spawn_caps
OS1	228	spawn_level
OS1	234	OS1low_handle_create
OS1	238	OS1low_handle_duplicate
OS1	243	OS1low_cap_grant
OS1	246	OS1_object_read
OS1	249	OS1_object_write
OS1	252	OS1_object_wait
OS1	255	OS1_object_ctl
OS1	263	OS1_window_enum
OS1	269	OS1_sys_stats
OS1	288	OS1_window_minimize
OS1	291	OS1_window_restore
OS1	294	OS1_window_focus
OS1	297	OS1_window_close
OS1	306	OS1_window_create
OS1	309	OS1_window_destroy
OS1	310	OS1_window_draw
OS1	314	OS1_window_blit
OS1	319	OS1_window_write
OS1	323	OS1_window_grid
OS1	333	OS1_window_set_flags
OS1	336	OS1_window_set_focus
OS1	340	OS1_window_resize
OS1	343	OS1_gfx_draw
OS1	348	OS1_gfx_flush
OS1	349	OS1_gfx_render
OS1	353	OS1_identity
OS1	383	yield
OS1	389	OS1_sleep
OS1	402	OS1low_ipc_send
OS1	405	OS1low_ipc_recv
OS1	408	OS1low_ipc_try_recv
OS1	412	send
OS1	415	recv
OS1	466	OS1_registry_get
OS1	469	OS1_registry_set
OS1	483	OS1_registry_enum
OS1	487	OS1_registry_enum_under
OS1	491	OS1_registry_del
OS1	509	OS1_display_set_style
OS1	512	OS1_display_set_background
OS1	516	OS1_display_set_font
OS1	534	OS1_fs_write
OS1	562	OS1_fs_read
OS1	582	OS1_fs_list
OS1	788	OS1_notify_post
OS1	791	OS1_notify_warn
OS1	794	OS1_notify_error
POSIX	130	read
POSIX	133	write
POSIX	1451	mkdir
POSIX	147	clock_gettime
POSIX	1589	stat
POSIX	159	nanosleep
POSIX	1959	tcgetattr
POSIX	1966	tcsetattr
POSIX	2004	munmap
POSIX	2061	closedir
POSIX	394	usleep
POSIX	612	open
POSIX	623	lseek
TERM	1691	puts
TERM	664	printf_win
TERM	686	print
TERM	712	putchar
---
GRAPHICS: 17 simboli
TERM: 4 simboli
LIBC: 66 simboli
POSIX: 13 simboli
OS1: 56 simboli
IO: 1 simboli
```

---

## R0.2 — Alias bare-name: classificazione e call-site (2026-07-17)

### Da ELIMINARE in R3 (shadow di nomi OS1 canonici)

Matrice d'uso nelle app native (`user/bin/*.c` non portati + `user/sys/bin/**`),
ordinata per impatto — è l'input diretto delle patch coccinelle di R3:

| alias | call-site | nome canonico (destinazione) |
|---|---|---|
| print | 89 | TERM (nome da definire in R1, es. `OS1_term_print`) |
| printf_win | 32 | TERM (es. `OS1_term_printf_win`) |
| get_pid | 28 | `OS1low_process_self` |
| create_window | 25 | `OS1_window_create` (da introdurre: oggi shim → _sys) |
| yield | 21 | `OS1low_process_yield` |
| file_read | 19 | `OS1_fs_read` |
| spawn | 17 | `OS1low_process_spawn` |
| get_time | 17 | `OS1_time_now` |
| wait | 15 | `OS1low_process_wait` (⚠ firma diversa dal POSIX wait) |
| try_recv | 15 | `OS1_ipc_try_recv` (da introdurre) |
| compositor_render | 11 | `OS1_gfx_render` |
| window_draw | 10 | `OS1_window_draw` (da introdurre) |
| flush | 10 | `OS1_gfx_flush` |
| recv | 8 | `OS1_ipc_recv` (da introdurre) |
| destroy_window | 8 | `OS1_window_destroy` |
| send | 7 | `OS1_ipc_send` (da introdurre) |
| file_write | 6 | `OS1_fs_write` |
| set_focus | 5 | `OS1_window_set_focus` |
| list_dir | 5 | `OS1_fs_list` |
| kill_process | 5 | `OS1low_process_kill` |
| spawn_level | 4 | `OS1low_process_spawn_caps` |
| window_of_pid | 2 | `OS1_window_of_pid` |
| set_font | 2 | `OS1_gfx_set_font` (da introdurre) |
| spawn_caps | 0 | `OS1low_process_spawn_caps` |

Totale ≈ 361 call-site. Nomi canonici "da introdurre": oggi l'alias inoltra
direttamente a `_sys_*` senza un OS1_* intermedio — il nome OS1 va creato
nello split R2 prima delle riscritture R3.

### Nomi POSIX/C standard che RESTANO (diventano la superficie POSIX/LIBC)

`exit`, `chdir`, `getcwd`, `sbrk`, `open/read/write/close/lseek`, `stat`,
`mkdir`, `opendir/readdir/closedir`, `mmap/munmap`, `tcgetattr/tcsetattr`,
`nanosleep/usleep`, `clock_gettime` — non sono alias: sono l'API POSIX,
implementata SOPRA OS1lib_OS1.

## R0.3 — Mappa header + punti di aggancio build

### Inventario `include/api` (35 header + 5 in `sys/`)

**⚠ PUNTO CRITICO — 9 header sono l'ABI condivisa col KERNEL** (il build
kernel usa `-Iinclude/api`, Makefile:97): `syscall_nums.h`, `object.h`,
`caps.h`, `posix_types.h`, `os1.h`, `sysstats.h`, `style_names.h`,
`font.h`, `stdbool.h`. Spostarli sotto `user/sys/lib/` farebbe dipendere il
kernel dall'albero userland (direzione sbagliata). Proposta da confermare:
- header **ABI kernel↔userland** → posizione neutra `include/abi/`
  (contratto, nessuna implementazione);
- tutti gli **header di libreria** (stdio, stdlib, string, dirent, fcntl,
  unistd, termios, time, math, ctype, graphics, image, input, …) →
  `user/sys/lib/include/api/` come da direttiva.

### Punti Makefile/rootfs da aggiornare (R1.2)

- `Makefile:97` — `INCLUDE = … -Iinclude/api` (build kernel E userland)
- `Makefile:402` — SDL2 CFLAGS `-Iinclude/api`
- `Makefile:478` — Lua CFLAGS `-Iinclude/api`
- `Makefile:715-716` — copia header nel rootfs:
  `cp -r include/api/. $(BUILD_DIR)/rootfs/sys/lib/include/api/`
