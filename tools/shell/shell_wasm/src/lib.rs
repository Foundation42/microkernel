#![no_std]
#![no_main]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    let msg = b"panic in shell wasm\n";
    unsafe { mk_print(msg.as_ptr(), msg.len() as i32) };
    loop {}
}

// Host function imports
extern "C" {
    fn mk_send(dest: i64, msg_type: i32, payload: *const u8, size: i32) -> i32;
    fn mk_self() -> i64;
    #[allow(dead_code)]
    fn mk_recv(type_out: *mut u32, buf: *mut u8, buf_size: i32, size_out: *mut u32) -> i32;
    fn mk_recv_full(
        type_out: *mut u32, buf: *mut u8, buf_size: i32,
        size_out: *mut u32, source_out: *mut i64,
    ) -> i32;
    fn mk_recv_timeout(
        type_out: *mut u32, buf: *mut u8, buf_size: i32,
        size_out: *mut u32, source_out: *mut i64, timeout_ms: i32,
    ) -> i32;
    fn mk_print(text: *const u8, len: i32);
    fn mk_stop(id: i64) -> i32;
    fn mk_list_actors(buf: *mut i64, max: i32, count_out: *mut u32) -> i32;
    fn mk_register(name: *const u8, len: i32) -> i32;
    fn mk_lookup(name: *const u8, len: i32) -> i64;
    fn mk_read_file(
        path: *const u8, path_len: i32,
        buf: *mut u8, buf_size: i32,
        size_out: *mut u32,
    ) -> i32;
    fn mk_http_get(
        url: *const u8, url_len: i32,
        buf: *mut u8, buf_size: i32,
        status_out: *mut u32, size_out: *mut u32,
    ) -> i32;
}

const ACTOR_ID_INVALID: i64 = 0;
const MSG_SHELL_INPUT: u32 = 100;
const MSG_INIT: u32 = 101;
#[allow(dead_code)]
const MSG_SPAWN_REQUEST: u32 = 102;
const MSG_SPAWN_RESPONSE: u32 = 103;
const MSG_SPAWN_REQUEST_NAMED: u32 = 104;

// Buffer sizes
const INPUT_BUF_SIZE: usize = 1024;
const FILE_BUF_SIZE: usize = 32768; // 32KB — keeps WASM to 1 page (64KB)

// Static buffers (avoid need for allocator).
// Safety: single-threaded WASM execution, no concurrent access.
static mut INPUT_BUF: [u8; INPUT_BUF_SIZE] = [0u8; INPUT_BUF_SIZE];
static mut FILE_BUF: [u8; FILE_BUF_SIZE] = [0u8; FILE_BUF_SIZE];

fn print(s: &[u8]) {
    unsafe { mk_print(s.as_ptr(), s.len() as i32) };
}

fn print_str(s: &str) {
    print(s.as_bytes());
}

fn print_u64(mut n: u64) {
    if n == 0 {
        print(b"0");
        return;
    }
    let mut buf = [0u8; 20];
    let mut i = 20;
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    print(&buf[i..20]);
}

fn print_i32(n: i32) {
    if n < 0 {
        print(b"-");
        print_u64((-n) as u64);
    } else {
        print_u64(n as u64);
    }
}

fn trim(s: &[u8]) -> &[u8] {
    let mut start = 0;
    let mut end = s.len();
    while start < end && matches!(s[start], b' ' | b'\t' | b'\n' | b'\r') {
        start += 1;
    }
    while end > start && matches!(s[end - 1], b' ' | b'\t' | b'\n' | b'\r') {
        end -= 1;
    }
    &s[start..end]
}

fn starts_with(haystack: &[u8], needle: &[u8]) -> bool {
    haystack.len() >= needle.len() && &haystack[..needle.len()] == needle
}

fn parse_u64(s: &[u8]) -> Option<u64> {
    if s.is_empty() {
        return None;
    }
    let mut n: u64 = 0;
    for &b in s {
        if b < b'0' || b > b'9' {
            return None;
        }
        n = n.wrapping_mul(10).wrapping_add((b - b'0') as u64);
    }
    Some(n)
}

fn parse_i32(s: &[u8]) -> Option<i32> {
    parse_u64(s).map(|n| n as i32)
}

fn split_first_space(s: &[u8]) -> (&[u8], &[u8]) {
    for i in 0..s.len() {
        if s[i] == b' ' || s[i] == b'\t' {
            let rest = trim(&s[i + 1..]);
            return (&s[..i], rest);
        }
    }
    (s, b"")
}

/// Resolve a target argument: try name lookup first, fall back to numeric ID.
fn resolve_target(arg: &[u8]) -> Option<i64> {
    let id = unsafe { mk_lookup(arg.as_ptr(), arg.len() as i32) };
    if id != ACTOR_ID_INVALID {
        return Some(id);
    }
    parse_u64(arg).map(|n| n as i64)
}

/// Extract a base name from a file path or URL.
/// "/spiffs/echo.wasm" -> "echo", "http://host/modules/foo.wasm?v=2" -> "foo"
fn extract_name(path: &[u8]) -> &[u8] {
    // Find last '/'
    let mut start = 0;
    for i in 0..path.len() {
        if path[i] == b'/' {
            start = i + 1;
        }
    }
    let filename = &path[start..];

    // Truncate at '?' (query string)
    let mut end = filename.len();
    for i in 0..filename.len() {
        if filename[i] == b'?' {
            end = i;
            break;
        }
    }
    let filename = &filename[..end];

    // Strip .wasm extension
    if filename.len() > 5 && &filename[filename.len() - 5..] == b".wasm" {
        &filename[..filename.len() - 5]
    } else {
        filename
    }
}

fn print_msg_payload(buf: &[u8], size: u32) {
    let len = core::cmp::min(size as usize, buf.len());
    let display_len = core::cmp::min(len, 64);
    if display_len > 0 {
        print_str(" \"");
        print(&buf[..display_len]);
        if len > display_len {
            print_str("...");
        }
        print_str("\"");
    }
}

fn cmd_help() {
    print_str("Commands:\n");
    print_str("  help                              Show this help\n");
    print_str("  list                              List active actors\n");
    print_str("  load <path-or-url>                Load WASM actor from file or URL\n");
    print_str("  send <name-or-id> <type> [data]   Send message to actor\n");
    print_str("  call <name-or-id> <type> [data]   Send and wait for reply (5s)\n");
    print_str("  stop <name-or-id>                 Stop an actor\n");
    print_str("  register <name>                   Register self by name\n");
    print_str("  lookup <name>                     Lookup actor by name\n");
    print_str("  self                              Print own actor ID\n");
    print_str("  exit                              Shut down\n");
}

fn cmd_list() {
    let mut ids = [0i64; 64];
    let mut count: u32 = 0;
    let rc = unsafe { mk_list_actors(ids.as_mut_ptr(), 64, &mut count) };
    if rc < 0 {
        print_str("error: mk_list_actors failed\n");
        return;
    }
    print_str("Active actors (");
    print_u64(count as u64);
    print_str("):\n");
    for i in 0..count as usize {
        print_str("  ");
        print_u64(ids[i] as u64);
        print_str("\n");
    }
}

fn cmd_load(arg: &[u8]) {
    if arg.is_empty() {
        print_str("usage: load <path-or-url>\n");
        return;
    }

    let is_url = starts_with(arg, b"http://") || starts_with(arg, b"https://");

    let file_buf = unsafe { &mut *core::ptr::addr_of_mut!(FILE_BUF) };
    let mut size_out: u32 = 0;

    if is_url {
        let mut status: u32 = 0;
        let rc = unsafe {
            mk_http_get(
                arg.as_ptr(), arg.len() as i32,
                file_buf.as_mut_ptr(), file_buf.len() as i32,
                &mut status, &mut size_out,
            )
        };
        if rc < 0 {
            print_str("error: HTTP GET failed\n");
            return;
        }
        if status != 200 {
            print_str("error: HTTP ");
            print_u64(status as u64);
            print_str("\n");
            return;
        }
        print_str("Downloaded ");
        print_u64(size_out as u64);
        print_str(" bytes\n");
    } else {
        let rc = unsafe {
            mk_read_file(
                arg.as_ptr(), arg.len() as i32,
                file_buf.as_mut_ptr(), file_buf.len() as i32,
                &mut size_out,
            )
        };
        if rc < 0 {
            print_str("error: cannot read file\n");
            return;
        }
        print_str("Read ");
        print_u64(size_out as u64);
        print_str(" bytes from file\n");
    }

    if size_out == 0 {
        print_str("error: empty file/response\n");
        return;
    }

    // Extract name from path/URL for auto-registration
    let name = extract_name(arg);
    let name_len = if name.len() > 63 { 63 } else { name.len() };
    let prefix_len = 1 + name_len;

    if (size_out as usize) + prefix_len > file_buf.len() {
        print_str("error: file too large for name prefix\n");
        return;
    }

    // Shift WASM bytes right to make room for name prefix
    unsafe {
        core::ptr::copy(
            file_buf.as_ptr(),
            file_buf.as_mut_ptr().add(prefix_len),
            size_out as usize,
        );
        file_buf[0] = name_len as u8;
        core::ptr::copy_nonoverlapping(
            name.as_ptr(),
            file_buf.as_mut_ptr().add(1),
            name_len,
        );
    }

    // Send named spawn request to console actor
    let console = unsafe { mk_lookup(b"console\0".as_ptr(), 7) };
    if console == ACTOR_ID_INVALID {
        print_str("error: console actor not found\n");
        return;
    }

    let total = (size_out as usize + prefix_len) as i32;
    let sent = unsafe {
        mk_send(console, MSG_SPAWN_REQUEST_NAMED as i32,
                file_buf.as_ptr(), total)
    };
    if sent == 0 {
        print_str("error: failed to send spawn request\n");
        return;
    }
    print_str("Loading...\n");
}

fn handle_spawn_response(payload: *const u8, size: u32) {
    if size < 8 || payload.is_null() {
        print_str("error: spawn failed (bad response)\n");
        return;
    }
    let mut bytes = [0u8; 8];
    unsafe { core::ptr::copy_nonoverlapping(payload, bytes.as_mut_ptr(), 8) };
    let id = i64::from_ne_bytes(bytes);
    if id == ACTOR_ID_INVALID {
        print_str("error: spawn failed\n");
    } else {
        print_str("Spawned actor ");
        print_u64(id as u64);
        if size > 8 {
            let name_len = (size - 8) as usize;
            let copy_len = if name_len > 63 { 63 } else { name_len };
            let mut name_buf = [0u8; 64];
            unsafe {
                core::ptr::copy_nonoverlapping(
                    payload.add(8),
                    name_buf.as_mut_ptr(),
                    copy_len,
                );
            }
            print_str(" as '");
            print(&name_buf[..copy_len]);
            print_str("'");
        }
        print_str("\n");
    }
}

fn cmd_send(arg: &[u8]) {
    let (target_str, rest) = split_first_space(arg);
    if target_str.is_empty() {
        print_str("usage: send <name-or-id> <type> [payload]\n");
        return;
    }
    let dest = match resolve_target(target_str) {
        Some(v) => v,
        None => {
            print_str("error: unknown target '");
            print(target_str);
            print_str("'\n");
            return;
        }
    };

    let (type_str, payload) = split_first_space(rest);
    if type_str.is_empty() {
        print_str("usage: send <name-or-id> <type> [payload]\n");
        return;
    }
    let msg_type = match parse_i32(type_str) {
        Some(v) => v,
        None => {
            print_str("error: invalid message type\n");
            return;
        }
    };

    let rc = if payload.is_empty() {
        unsafe { mk_send(dest, msg_type, core::ptr::null(), 0) }
    } else {
        unsafe { mk_send(dest, msg_type, payload.as_ptr(), payload.len() as i32) }
    };

    if rc != 0 {
        print_str("Sent type=");
        print_i32(msg_type);
        print_str(" to actor ");
        print_u64(dest as u64);
        print_str("\n");
    } else {
        print_str("error: send failed\n");
    }
}

fn cmd_call(arg: &[u8]) {
    let (target_str, rest) = split_first_space(arg);
    if target_str.is_empty() {
        print_str("usage: call <name-or-id> <type> [payload]\n");
        return;
    }
    let dest = match resolve_target(target_str) {
        Some(v) => v,
        None => {
            print_str("error: unknown target '");
            print(target_str);
            print_str("'\n");
            return;
        }
    };

    let (type_str, payload) = split_first_space(rest);
    if type_str.is_empty() {
        print_str("usage: call <name-or-id> <type> [payload]\n");
        return;
    }
    let msg_type = match parse_i32(type_str) {
        Some(v) => v,
        None => {
            print_str("error: invalid message type\n");
            return;
        }
    };

    // Send
    let rc = if payload.is_empty() {
        unsafe { mk_send(dest, msg_type, core::ptr::null(), 0) }
    } else {
        unsafe { mk_send(dest, msg_type, payload.as_ptr(), payload.len() as i32) }
    };
    if rc == 0 {
        print_str("error: send failed\n");
        return;
    }

    // Wait for reply with 5s timeout
    let input_buf = unsafe { &mut *core::ptr::addr_of_mut!(INPUT_BUF) };
    let mut recv_type: u32 = 0;
    let mut recv_size: u32 = 0;
    let mut recv_source: i64 = 0;
    let rc = unsafe {
        mk_recv_timeout(
            &mut recv_type,
            input_buf.as_mut_ptr(),
            input_buf.len() as i32,
            &mut recv_size,
            &mut recv_source,
            5000,
        )
    };

    if rc == -2 {
        print_str("Timeout (5s)\n");
        return;
    }
    if rc < 0 {
        print_str("error: recv failed\n");
        return;
    }

    print_str("[reply] type=");
    print_u64(recv_type as u64);
    print_str(" from=");
    print_u64(recv_source as u64);
    print_str(" size=");
    print_u64(recv_size as u64);
    print_msg_payload(input_buf, recv_size);
    print_str("\n");
}

fn cmd_stop(arg: &[u8]) {
    if arg.is_empty() {
        print_str("usage: stop <name-or-id>\n");
        return;
    }
    let id = match resolve_target(arg) {
        Some(v) => v,
        None => {
            print_str("error: unknown target '");
            print(arg);
            print_str("'\n");
            return;
        }
    };
    unsafe { mk_stop(id) };
    print_str("Stopped actor ");
    print_u64(id as u64);
    print_str("\n");
}

fn cmd_register(arg: &[u8]) {
    if arg.is_empty() {
        print_str("usage: register <name>\n");
        return;
    }
    let rc = unsafe { mk_register(arg.as_ptr(), arg.len() as i32) };
    if rc == 0 {
        print_str("Registered as '");
        print(arg);
        print_str("'\n");
    } else {
        print_str("error: register failed\n");
    }
}

fn cmd_lookup(arg: &[u8]) {
    if arg.is_empty() {
        print_str("usage: lookup <name>\n");
        return;
    }
    let id = unsafe { mk_lookup(arg.as_ptr(), arg.len() as i32) };
    if id == ACTOR_ID_INVALID {
        print_str("Not found\n");
    } else {
        print_str("Actor ");
        print_u64(id as u64);
        print_str("\n");
    }
}

fn cmd_self_id() {
    let id = unsafe { mk_self() };
    print_str("Self: ");
    print_u64(id as u64);
    print_str("\n");
}

fn dispatch_command(line: &[u8]) {
    let trimmed = trim(line);
    if trimmed.is_empty() {
        return;
    }

    let (cmd, arg) = split_first_space(trimmed);

    if cmd == b"help" || cmd == b"?" {
        cmd_help();
    } else if cmd == b"list" || cmd == b"ls" {
        cmd_list();
    } else if cmd == b"load" {
        cmd_load(arg);
    } else if cmd == b"send" {
        cmd_send(arg);
    } else if cmd == b"call" {
        cmd_call(arg);
    } else if cmd == b"stop" {
        cmd_stop(arg);
    } else if cmd == b"register" {
        cmd_register(arg);
    } else if cmd == b"lookup" {
        cmd_lookup(arg);
    } else if cmd == b"self" {
        cmd_self_id();
    } else if cmd == b"exit" || cmd == b"quit" {
        // Handled by caller — won't reach here
    } else {
        print_str("Unknown command: ");
        print(cmd);
        print_str("\nType 'help' for available commands.\n");
    }
}

#[no_mangle]
pub extern "C" fn handle_message(
    msg_type: i32,
    _source_id: i64,
    _payload: *const u8,
    _payload_size: i32,
) -> i32 {
    let msg_type = msg_type as u32;

    if msg_type == MSG_INIT {
        print_str("╔════════════════════════════════════╗\n");
        print_str("║  microkernel WASM shell v0.2       ║\n");
        print_str("║  Type 'help' for commands          ║\n");
        print_str("╚════════════════════════════════════╝\n");

        let input_buf = unsafe { &mut *core::ptr::addr_of_mut!(INPUT_BUF) };

        // REPL loop: receive messages via mk_recv_full
        loop {
            print_str("mk> ");

            let mut recv_type: u32 = 0;
            let mut recv_size: u32 = 0;
            let mut recv_source: i64 = 0;
            let rc = unsafe {
                mk_recv_full(
                    &mut recv_type,
                    input_buf.as_mut_ptr(),
                    input_buf.len() as i32,
                    &mut recv_size,
                    &mut recv_source,
                )
            };
            if rc < 0 {
                print_str("error: mk_recv failed\n");
                break;
            }

            if recv_type == MSG_SPAWN_RESPONSE {
                handle_spawn_response(input_buf.as_ptr(), recv_size);
                continue;
            }

            if recv_type != MSG_SHELL_INPUT {
                // Display unsolicited message
                print_str("[msg] type=");
                print_u64(recv_type as u64);
                print_str(" from=");
                print_u64(recv_source as u64);
                print_str(" size=");
                print_u64(recv_size as u64);
                print_msg_payload(input_buf, recv_size);
                print_str("\n");
                continue;
            }

            let len = core::cmp::min(recv_size as usize, input_buf.len());
            let line = &input_buf[..len];
            let trimmed = trim(line);

            if trimmed == b"exit" || trimmed == b"quit" {
                print_str("Goodbye.\n");
                return 0;
            }

            dispatch_command(trimmed);
        }

        return 0;
    }

    // Non-init messages: stay alive
    1
}
