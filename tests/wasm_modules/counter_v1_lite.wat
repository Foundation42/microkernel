(module
  ;; Import mk_send_u32(dest: i64, type: i32, value: i32) -> i32
  (import "env" "mk_send_u32" (func $mk_send_u32 (param i64 i32 i32) (result i32)))

  ;; WASM global — no linear memory needed
  (global $count (mut i32) (i32.const 0))

  ;; handle_message(type: i32, source: i64, payload: i32, size: i32) -> i32
  (func (export "handle_message") (param $type i32) (param $source i64) (param $payload i32) (param $size i32) (result i32)
    ;; MSG_INCREMENT (213): count += 1
    (if (i32.eq (local.get $type) (i32.const 213))
      (then
        (global.set $count (i32.add (global.get $count) (i32.const 1)))
        (return (i32.const 1))
      )
    )
    ;; MSG_GET_COUNT (211): reply with count
    (if (i32.eq (local.get $type) (i32.const 211))
      (then
        (drop (call $mk_send_u32 (local.get $source) (i32.const 212) (global.get $count)))
        (return (i32.const 1))
      )
    )
    ;; MSG_GET_VERSION (214): reply with version 1
    (if (i32.eq (local.get $type) (i32.const 214))
      (then
        (drop (call $mk_send_u32 (local.get $source) (i32.const 215) (i32.const 1)))
        (return (i32.const 1))
      )
    )
    ;; MSG_STOP (0): exit
    (if (i32.eq (local.get $type) (i32.const 0))
      (then (return (i32.const 0)))
    )
    (i32.const 1)
  )
)
