#![allow(unused_imports, dead_code, non_upper_case_globals)]

use std::cell::UnsafeCell;
use std::collections::{HashMap, HashSet};
use std::ffi::CString;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Poll, Waker, RawWaker, RawWakerVTable};
struct GlobalCell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for GlobalCell<T> {}
impl<T> GlobalCell<T> {
        const fn new(v: T) -> Self { Self(UnsafeCell::new(v)) }
        #[inline]
        fn get(&self) -> *mut T { self.0.get() }
}

pub struct Global<T> {
        init: fn() -> T,
        cell: std::cell::OnceCell<std::cell::RefCell<T>>,
}
unsafe impl<T> Sync for Global<T> {}
impl<T> Global<T> {
        pub const fn new(init: fn() -> T) -> Self { Self { init, cell: std::cell::OnceCell::new() } }
        fn cell(&self) -> &std::cell::RefCell<T> { self.cell.get_or_init(|| std::cell::RefCell::new((self.init)())) }
        pub fn borrow(&self) -> std::cell::Ref<'_, T> { self.cell().borrow() }
        pub fn borrow_mut(&self) -> std::cell::RefMut<'_, T> { self.cell().borrow_mut() }
}

static CTX: GlobalCell<Option<Context>> = GlobalCell::new(None);
static CORO_WAIT_MS: GlobalCell<i64> = GlobalCell::new(0);

#[inline]
fn ctx() -> *mut Context {
        unsafe { (*CTX.get()).as_mut().expect("cfx: context not initialized") as *mut Context }
}

pub const fn hash_string(s: &[u8]) -> u64 {
        let mut hash: u32 = 0;
        let mut i = 0;
        while i < s.len() {
                let c = s[i];
                let v = if c >= b'A' && c <= b'Z' { (c | 0x20) as u32 } else { c as u32 };
                hash = hash.wrapping_add(v);
                hash = hash.wrapping_add(hash << 10);
                hash ^= hash >> 6;
                i += 1;
        }
        hash = hash.wrapping_add(hash << 3);
        hash ^= hash >> 11;
        hash = hash.wrapping_add(hash << 15);
        hash as u64
}

fn allocate_id(next: &mut i32, used: &impl ContainsKey) -> i32 {
        let start = *next;
        let mut id = start;
        while used.contains(id) {
                id += 1;
                if id <= 0 { id = 1; }
                if id == start { return -1; }
        }
        *next = id + 1;
        if *next <= 0 { *next = 1; }
        id
}

trait ContainsKey { fn contains(&self, k: i32) -> bool; }
impl<V> ContainsKey for HashMap<i32, V> { fn contains(&self, k: i32) -> bool { self.contains_key(&k) } }
#[link(wasm_import_module = "cfx")]
extern "C" {
        #[link_name = "trace"]
        fn __cfx_trace(ptr: *const u8, len: u32);
        #[link_name = "invoke_native"]
        fn __cfx_invoke_native(ctx_ptr: u32);
        #[link_name = "copy_string_result"]
        fn __cfx_copy_string_result(ctx_ptr: u32, result_idx: i32, buf: *mut u8, buf_max: i32) -> i32;
        #[link_name = "copy_binary_result"]
        fn __cfx_copy_binary_result(ctx_ptr: u32, ptr_idx: i32, size_idx: i32, buf: *mut u8, buf_max: i32) -> i32;
        #[link_name = "emit_event"]
        fn __cfx_emit_event(name: *const u8, name_len: u32, args: *const u8, args_len: u32);
        #[link_name = "emit_net_event"]
        fn __cfx_emit_net_event(name: *const u8, name_len: u32, target: i32, args: *const u8, args_len: u32);
        #[link_name = "cancel_event"]
        fn __cfx_cancel_event();
        #[link_name = "was_event_canceled"]
        fn __cfx_was_event_canceled() -> i32;
        #[link_name = "get_resource_metadata"]
        fn __cfx_get_resource_metadata(key: *const u8, key_len: u32, index: i32, buf: *mut u8, buf_max: i32) -> i32;
        #[link_name = "get_num_resource_metadata"]
        fn __cfx_get_num_resource_metadata(key: *const u8, key_len: u32) -> i32;
        #[link_name = "create_ref"]
        fn __cfx_create_ref(callback_id: i32) -> i32;
        #[link_name = "canonicalize_ref"]
        fn __cfx_canonicalize_ref(ref_idx: i32, buf: *mut u8, buf_max: i32) -> i32;
        #[link_name = "remove_ref"]
        fn __cfx_remove_ref(ref_idx: i32);
        #[link_name = "invoke_function_reference"]
        fn __cfx_invoke_function_reference(ref_str: *const u8, ref_len: u32, args: *const u8, args_len: u32, out: *mut [u32; 2]);
        #[link_name = "get_instance_id"]
        fn __cfx_get_instance_id() -> i32;
        #[link_name = "get_last_native_error"]
        fn __cfx_get_last_native_error(buf: *mut u8, buf_max: i32) -> i32;
        #[link_name = "spawn_process"]
        fn __cfx_spawn_process(cmd: *const u8, cmd_len: u32) -> i32;
        #[link_name = "poll_process"]
        fn __cfx_poll_process(process_id: i32, out_buf: *mut u8, out_buf_max: i32, info: *mut [i32; 2]) -> i32;
        #[link_name = "create_worker"]
        fn __cfx_create_worker(fn_name: *const u8, fn_name_len: u32, input: *const u8, input_len: u32) -> i32;
        #[link_name = "poll_worker"]
        fn __cfx_poll_worker(worker_id: i32, out_buf: *mut u8, out_buf_max: i32) -> i32;
        #[link_name = "schedule_bookmark"]
        fn __cfx_schedule_bookmark(bookmark_id: i32, deadline_ms: i32);
        #[link_name = "is_manifest_version_v2_between"]
        fn __cfx_is_manifest_version_v2_between(lower: *const u8, lower_len: u32, upper: *const u8, upper_len: u32) -> i32;
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vector2 {
        pub x: f32,
        pub y: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vector3 {
        pub x: f32,
        pub y: f32,
        pub z: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vector4 {
        pub x: f32,
        pub y: f32,
        pub z: f32,
        pub w: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Quaternion {
        pub x: f32,
        pub y: f32,
        pub z: f32,
        pub w: f32,
}

#[repr(C)]
pub struct NativeCtx {
        pub hash: u64,
        pub num_args: u32,
        pub num_results: u32,
        pub args: [u64; 32],
        pub ptr_mask: u32,
        pub result_ptr_mask: u32,
}
const _: () = assert!(core::mem::size_of::<NativeCtx>() == 280);

impl NativeCtx {
        pub fn new(hash: u64) -> Self {
                Self { hash, num_args: 0, num_results: 0, args: [0u64; 32], ptr_mask: 0, result_ptr_mask: 0 }
        }
}

#[derive(Clone, Copy)]
pub struct NativeArg {
        pub value: u64,
        pub is_ptr: bool,
}

impl NativeArg {
        pub const fn val(v: u64) -> Self { Self { value: v, is_ptr: false } }
        pub const fn ptr(v: u64) -> Self { Self { value: v, is_ptr: true } }
}

struct FuncRefOwner {
        id: String,
        owned: bool,
}

static PENDING_REF_RELEASES: GlobalCell<Vec<String>> = GlobalCell::new(Vec::new());

impl Drop for FuncRefOwner {
        fn drop(&mut self) {
                if self.owned {
                        unsafe { (*PENDING_REF_RELEASES.get()).push(std::mem::take(&mut self.id)) };
                }
        }
}

fn release_pending_refs() {
        loop {
                let refs = unsafe { std::mem::take(&mut *PENDING_REF_RELEASES.get()) };
                if refs.is_empty() { break; }
                for r in refs {
                        let id = CString::new(r).unwrap_or_default();
                        invoke_native_void(hash_string(b"DELETE_FUNCTION_REFERENCE"), &[NativeArg::ptr(id.as_ptr() as u64)]);
                }
        }
}

#[derive(Clone)]
pub struct FuncRef(Rc<FuncRefOwner>);

impl FuncRef {
        pub fn new(id: impl Into<String>) -> Self { FuncRef(Rc::new(FuncRefOwner { id: id.into(), owned: false })) }
        fn owned(id: String) -> Self {
                let owned = !id.is_empty();
                FuncRef(Rc::new(FuncRefOwner { id, owned }))
        }
        pub fn as_str(&self) -> &str { &self.0.id }
        fn acquire(id: String) -> Self {
                let cs = CString::new(id.as_str()).unwrap_or_default();
                let dup = invoke_native_string(hash_string(b"DUPLICATE_FUNCTION_REFERENCE"), &[NativeArg::ptr(cs.as_ptr() as u64)]);
                if dup.is_empty() { FuncRef::new(id) } else { FuncRef::owned(dup) }
        }
}

impl std::ops::Deref for FuncRef {
        type Target = str;
        fn deref(&self) -> &str { self.as_str() }
}

impl std::fmt::Debug for FuncRef {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { write!(f, "FuncRef({:?})", self.as_str()) }
}

#[derive(Clone, Debug)]
pub enum Value {
        Null,
        Bool(bool),
        Int(i64),
        Float(f64),
        String(String),
        Binary(Vec<u8>),
        Array(Vec<Value>),
        Object(Vec<(String, Value)>),
        FuncRef(FuncRef),
        Vector2(Vector2),
        Vector3(Vector3),
        Vector4(Vector4),
        Quaternion(Quaternion),
}

struct NullValue(Value);
unsafe impl Sync for NullValue {}
static NULL_VALUE: NullValue = NullValue(Value::Null);

impl Default for Value {
        fn default() -> Self { Value::Null }
}

impl std::ops::Index<&str> for Value {
        type Output = Value;
        fn index(&self, key: &str) -> &Value { self.get(key) }
}

impl std::ops::Index<&String> for Value {
        type Output = Value;
        fn index(&self, key: &String) -> &Value { self.get(key) }
}

impl std::ops::Index<usize> for Value {
        type Output = Value;
        fn index(&self, i: usize) -> &Value { self.at(i) }
}

impl Value {
        pub fn as_str(&self) -> &str {
                match self { Value::String(s) => s, _ => "" }
        }
        pub fn as_int(&self) -> i32 {
                match self { Value::Int(n) => *n as i32, Value::Float(n) => *n as i32, _ => 0 }
        }
        pub fn as_i64(&self) -> i64 {
                match self { Value::Int(n) => *n, Value::Float(n) => *n as i64, _ => 0 }
        }
        pub fn as_f64(&self) -> f64 {
                match self { Value::Float(n) => *n, Value::Int(n) => *n as f64, _ => 0.0 }
        }
        pub fn as_f32(&self) -> f32 { self.as_f64() as f32 }
        pub fn as_bool(&self) -> bool {
                match self { Value::Bool(b) => *b, _ => false }
        }
        pub fn is_null(&self) -> bool { matches!(self, Value::Null) }
        pub fn len(&self) -> usize {
                match self { Value::Array(v) => v.len(), Value::Object(v) => v.len(), _ => 0 }
        }
        pub fn is_empty(&self) -> bool { self.len() == 0 }
        pub fn at(&self, i: usize) -> &Value {
                match self { Value::Array(v) => v.get(i).unwrap_or(&NULL_VALUE.0), _ => &NULL_VALUE.0 }
        }
        pub fn get(&self, key: &str) -> &Value {
                match self {
                        Value::Object(entries) => {
                                for (k, v) in entries {
                                        if k == key { return v; }
                                }
                                &NULL_VALUE.0
                        }
                        _ => &NULL_VALUE.0,
                }
        }
        pub fn has(&self, key: &str) -> bool {
                match self { Value::Object(entries) => entries.iter().any(|(k, _)| k == key), _ => false }
        }
        pub fn is_func_ref(&self) -> bool { matches!(self, Value::FuncRef(_)) }
        pub fn call(&self, args: impl IntoArgs) -> Value {
                let r = match self { Value::FuncRef(r) => r, _ => return Value::Null };
                let data = invoke_function_reference(r, &encode(&Value::Array(args.into_args())));
                if data.is_empty() { return Value::Null; }
                match decode(&data) {
                        Value::Array(values) => values.into_iter().next().unwrap_or(Value::Null),
                        other => other,
                }
        }
        pub fn func_ref_str(&self) -> &str {
                match self { Value::FuncRef(s) => s.as_str(), _ => "" }
        }
        fn components(&self, n: usize, quaternion: bool) -> Option<[f32; 4]> {
                match (self, n, quaternion) {
                        (Value::Vector2(v), 2, false) => Some([v.x, v.y, 0.0, 0.0]),
                        (Value::Vector3(v), 3, false) => Some([v.x, v.y, v.z, 0.0]),
                        (Value::Vector4(v), 4, false) => Some([v.x, v.y, v.z, v.w]),
                        (Value::Quaternion(q), 4, true) => Some([q.x, q.y, q.z, q.w]),
                        (Value::Array(items), _, _) if items.len() == n => {
                                let mut out = [0.0f32; 4];
                                for (slot, item) in out.iter_mut().zip(items) {
                                        *slot = match item { Value::Int(i) => *i as f32, Value::Float(f) => *f as f32, _ => return None };
                                }
                                Some(out)
                        }
                        _ => None,
                }
        }
        pub fn as_vector2(&self) -> Vector2 {
                self.components(2, false).map_or_else(Vector2::default, |c| Vector2 { x: c[0], y: c[1] })
        }
        pub fn as_vector3(&self) -> Vector3 {
                self.components(3, false).map_or_else(Vector3::default, |c| Vector3 { x: c[0], y: c[1], z: c[2] })
        }
        pub fn as_vector4(&self) -> Vector4 {
                self.components(4, false).map_or_else(Vector4::default, |c| Vector4 { x: c[0], y: c[1], z: c[2], w: c[3] })
        }
        pub fn as_quaternion(&self) -> Quaternion {
                self.components(4, true).map_or_else(Quaternion::default, |c| Quaternion { x: c[0], y: c[1], z: c[2], w: c[3] })
        }
}

impl From<i8> for Value { fn from(v: i8) -> Self { Value::Int(v as i64) } }
impl From<i16> for Value { fn from(v: i16) -> Self { Value::Int(v as i64) } }
impl From<i32> for Value { fn from(v: i32) -> Self { Value::Int(v as i64) } }
impl From<i64> for Value { fn from(v: i64) -> Self { Value::Int(v) } }
impl From<u8> for Value { fn from(v: u8) -> Self { Value::Int(v as i64) } }
impl From<u16> for Value { fn from(v: u16) -> Self { Value::Int(v as i64) } }
impl From<u32> for Value { fn from(v: u32) -> Self { Value::Int(v as i64) } }
impl From<u64> for Value { fn from(v: u64) -> Self { if v > i64::MAX as u64 { Value::Float(v as f64) } else { Value::Int(v as i64) } } }
impl From<usize> for Value { fn from(v: usize) -> Self { Value::from(v as u64) } }
impl From<isize> for Value { fn from(v: isize) -> Self { Value::Int(v as i64) } }
impl<T: Into<Value>> From<Vec<T>> for Value { fn from(v: Vec<T>) -> Self { Value::Array(v.into_iter().map(Into::into).collect()) } }
impl<T: Into<Value>, const N: usize> From<[T; N]> for Value { fn from(v: [T; N]) -> Self { Value::Array(v.into_iter().map(Into::into).collect()) } }
impl<T: Into<Value>> From<Option<T>> for Value { fn from(v: Option<T>) -> Self { v.map_or(Value::Null, Into::into) } }
impl<K: Into<String>, T: Into<Value>> From<HashMap<K, T>> for Value { fn from(v: HashMap<K, T>) -> Self { Value::Object(v.into_iter().map(|(k, t)| (k.into(), t.into())).collect()) } }
impl<K: Into<String>, T: Into<Value>> From<std::collections::BTreeMap<K, T>> for Value { fn from(v: std::collections::BTreeMap<K, T>) -> Self { Value::Object(v.into_iter().map(|(k, t)| (k.into(), t.into())).collect()) } }
impl From<f64> for Value { fn from(v: f64) -> Self { Value::Float(v) } }
impl From<f32> for Value { fn from(v: f32) -> Self { Value::Float(v as f64) } }
impl From<bool> for Value { fn from(v: bool) -> Self { Value::Bool(v) } }
impl From<&str> for Value { fn from(v: &str) -> Self { Value::String(v.to_string()) } }
impl From<String> for Value { fn from(v: String) -> Self { Value::String(v) } }
impl From<()> for Value { fn from(_: ()) -> Self { Value::Null } }
impl From<&Value> for Value { fn from(v: &Value) -> Self { v.clone() } }
impl From<Vector2> for Value { fn from(v: Vector2) -> Self { Value::Vector2(v) } }
impl From<Vector3> for Value { fn from(v: Vector3) -> Self { Value::Vector3(v) } }
impl From<Vector4> for Value { fn from(v: Vector4) -> Self { Value::Vector4(v) } }
impl From<Quaternion> for Value { fn from(q: Quaternion) -> Self { Value::Quaternion(q) } }

pub trait IntoArgs {
        fn into_args(self) -> Vec<Value>;
}

impl IntoArgs for () { fn into_args(self) -> Vec<Value> { Vec::new() } }
impl IntoArgs for Vec<Value> { fn into_args(self) -> Vec<Value> { self } }
impl IntoArgs for &Vec<Value> { fn into_args(self) -> Vec<Value> { self.clone() } }
impl IntoArgs for &[Value] { fn into_args(self) -> Vec<Value> { self.to_vec() } }
impl<const N: usize> IntoArgs for [Value; N] { fn into_args(self) -> Vec<Value> { self.into() } }
impl<const N: usize> IntoArgs for &[Value; N] { fn into_args(self) -> Vec<Value> { self.to_vec() } }

macro_rules! impl_single_into_args {
        ($($t:ty),*) => { $(impl IntoArgs for $t { fn into_args(self) -> Vec<Value> { vec![self.into()] } })* };
}
impl_single_into_args!(Value, bool, i8, i16, i32, i64, u8, u16, u32, u64, isize, usize, f32, f64, String, &str, Vector2, Vector3, Vector4, Quaternion);

macro_rules! impl_tuple_into_args {
        ($($t:ident $i:tt),+) => {
                impl<$($t: Into<Value>),+> IntoArgs for ($($t,)+) {
                        fn into_args(self) -> Vec<Value> { vec![$(self.$i.into()),+] }
                }
        };
}
impl_tuple_into_args!(A 0);
impl_tuple_into_args!(A 0, B 1);
impl_tuple_into_args!(A 0, B 1, C 2);
impl_tuple_into_args!(A 0, B 1, C 2, D 3);
impl_tuple_into_args!(A 0, B 1, C 2, D 3, E 4);
impl_tuple_into_args!(A 0, B 1, C 2, D 3, E 4, F 5);
impl_tuple_into_args!(A 0, B 1, C 2, D 3, E 4, F 5, G 6);
impl_tuple_into_args!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7);
impl_tuple_into_args!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8);
impl_tuple_into_args!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8, J 9);
impl_tuple_into_args!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8, J 9, K 10);
impl_tuple_into_args!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8, J 9, K 10, L 11);
pub struct MsgpackWriter {
        pub buf: Vec<u8>,
}

impl MsgpackWriter {
        pub fn new() -> Self { Self { buf: Vec::new() } }

        fn pu8(&mut self, v: u8) { self.buf.push(v); }
        fn pu16(&mut self, v: u16) { self.buf.extend_from_slice(&v.to_be_bytes()); }
        fn pu32(&mut self, v: u32) { self.buf.extend_from_slice(&v.to_be_bytes()); }
        fn pu64(&mut self, v: u64) { self.buf.extend_from_slice(&v.to_be_bytes()); }

        pub fn enc_null(&mut self) { self.pu8(0xC0); }
        pub fn enc_bool(&mut self, v: bool) { self.pu8(if v { 0xC3 } else { 0xC2 }); }

        pub fn enc_int(&mut self, v: i64) {
                if v >= 0 && v <= 127 { self.pu8(v as u8); }
                else if v < 0 && v >= -32 { self.pu8(v as u8); }
                else if v >= -128 && v <= 127 { self.pu8(0xD0); self.pu8(v as u8); }
                else if v >= -32768 && v <= 32767 { self.pu8(0xD1); self.pu16(v as u16); }
                else if v >= -2147483648 && v <= 2147483647 { self.pu8(0xD2); self.pu32(v as u32); }
                else { self.pu8(0xD3); self.pu64(v as u64); }
        }

        pub fn enc_double(&mut self, d: f64) {
                self.pu8(0xCB);
                self.pu64(d.to_bits());
        }

        pub fn enc_str(&mut self, s: &str) {
                let n = s.len() as u32;
                if n <= 31 { self.pu8(0xA0 | n as u8); }
                else if n <= 255 { self.pu8(0xD9); self.pu8(n as u8); }
                else if n <= 65535 { self.pu8(0xDA); self.pu16(n as u16); }
                else { self.pu8(0xDB); self.pu32(n); }
                self.buf.extend_from_slice(s.as_bytes());
        }

        pub fn enc_bin(&mut self, data: &[u8]) {
                let n = data.len() as u32;
                if n <= 255 { self.pu8(0xC4); self.pu8(n as u8); }
                else if n <= 65535 { self.pu8(0xC5); self.pu16(n as u16); }
                else { self.pu8(0xC6); self.pu32(n); }
                self.buf.extend_from_slice(data);
        }

        pub fn array_header(&mut self, n: u32) {
                if n <= 15 { self.pu8(0x90 | n as u8); }
                else if n <= 65535 { self.pu8(0xDC); self.pu16(n as u16); }
                else { self.pu8(0xDD); self.pu32(n); }
        }

        pub fn map_header(&mut self, n: u32) {
                if n <= 15 { self.pu8(0x80 | n as u8); }
                else if n <= 65535 { self.pu8(0xDE); self.pu16(n as u16); }
                else { self.pu8(0xDF); self.pu32(n); }
        }

        pub fn enc_value(&mut self, v: &Value) {
                match v {
                        Value::Null => self.enc_null(),
                        Value::Bool(b) => self.enc_bool(*b),
                        Value::Int(n) => self.enc_int(*n),
                        Value::Float(n) => self.enc_double(*n),
                        Value::String(s) => self.enc_str(s),
                        Value::Binary(b) => self.enc_bin(b),
                        Value::Array(arr) => {
                                self.array_header(arr.len() as u32);
                                for v in arr { self.enc_value(v); }
                        }
                        Value::Object(entries) => {
                                self.map_header(entries.len() as u32);
                                for (k, v) in entries { self.enc_str(k); self.enc_value(v); }
                        }
                        Value::FuncRef(r) => {
                                let s = r.as_str();
                                let n = s.len() as u32;
                                if n <= 255 { self.pu8(0xC7); self.pu8(n as u8); }
                                else if n <= 65535 { self.pu8(0xC8); self.pu16(n as u16); }
                                else { self.pu8(0xC9); self.pu32(n); }
                                self.pu8(10);
                                self.buf.extend_from_slice(s.as_bytes());
                        }
                        Value::Vector2(v) => self.enc_vector(20, &[v.x, v.y]),
                        Value::Vector3(v) => self.enc_vector(21, &[v.x, v.y, v.z]),
                        Value::Vector4(v) => self.enc_vector(22, &[v.x, v.y, v.z, v.w]),
                        Value::Quaternion(q) => self.enc_vector(23, &[q.x, q.y, q.z, q.w]),
                }
        }

        fn enc_vector(&mut self, ext_type: u8, components: &[f32]) {
                match components.len() * 4 {
                        8 => self.pu8(0xD7),
                        16 => self.pu8(0xD8),
                        size => { self.pu8(0xC7); self.pu8(size as u8); }
                }
                self.pu8(ext_type);
                for c in components { self.buf.extend_from_slice(&c.to_le_bytes()); }
        }
}

pub fn encode(v: &Value) -> Vec<u8> {
        let mut w = MsgpackWriter::new();
        w.enc_value(v);
        w.buf
}

pub fn encode_args(vals: &[Value]) -> Vec<u8> {
        let mut w = MsgpackWriter::new();
        w.array_header(vals.len() as u32);
        for v in vals { w.enc_value(v); }
        w.buf
}
struct MsgpackReader<'a> {
        data: &'a [u8],
        pos: usize,
        error: bool,
}

impl<'a> MsgpackReader<'a> {
        fn new(data: &'a [u8]) -> Self { Self { data, pos: 0, error: false } }

        fn u8(&mut self) -> u8 {
                if self.pos >= self.data.len() { self.error = true; return 0; }
                let v = self.data[self.pos]; self.pos += 1; v
        }
        fn u16(&mut self) -> u16 { let a = self.u8() as u16; let b = self.u8() as u16; (a << 8) | b }
        fn u32(&mut self) -> u32 { let a = self.u16() as u32; let b = self.u16() as u32; (a << 16) | b }
        fn u64(&mut self) -> u64 { let a = self.u32() as u64; let b = self.u32() as u64; (a << 32) | b }

        fn bytes(&mut self, n: usize) -> &'a [u8] {
                if n > self.data.len() - self.pos { self.error = true; return &[]; }
                let s = &self.data[self.pos..self.pos + n]; self.pos += n; s
        }

        fn read_str(&mut self, n: usize) -> String {
                String::from_utf8_lossy(self.bytes(n)).into_owned()
        }

        fn key_name(key: Value, index: u32) -> String {
                match key {
                        Value::String(s) => s,
                        Value::Int(n) => n.to_string(),
                        Value::Float(f) => f.to_string(),
                        Value::Bool(b) => b.to_string(),
                        _ => index.to_string(),
                }
        }

        fn read_map(&mut self, n: u32, depth: i32) -> Value {
                if n > 65536 { self.error = true; return Value::Null; }
                if n == 1 {
                        let key = self.read(depth + 1);
                        let val = self.read(depth + 1);
                        if let Value::String(ref s) = key {
                                if s == "__cfx_functionReference" {
                                        return Value::FuncRef(FuncRef::new(val.as_str()));
                                }
                        }
                        return Value::Object(vec![(Self::key_name(key, 0), val)]);
                }
                let mut entries = Vec::new();
                for i in 0..n {
                        if self.error { break; }
                        let key = self.read(depth + 1);
                        let val = self.read(depth + 1);
                        entries.push((Self::key_name(key, i), val));
                }
                Value::Object(entries)
        }

        fn read_ext(&mut self, n: usize) -> Value {
                let t = self.u8();
                if t == 10 {
                        let id = self.read_str(n);
                        return if self.error { Value::Null } else { Value::FuncRef(FuncRef::acquire(id)) };
                }
                let data = self.bytes(n);
                let count = match t { 20 => 2, 21 => 3, 22 | 23 => 4, _ => 0 };
                if count == 0 || data.len() != count * 4 { return Value::Null; }
                let c: Vec<f32> = data.chunks_exact(4).map(|b| f32::from_le_bytes([b[0], b[1], b[2], b[3]])).collect();
                match t {
                        20 => Value::Vector2(Vector2 { x: c[0], y: c[1] }),
                        21 => Value::Vector3(Vector3 { x: c[0], y: c[1], z: c[2] }),
                        22 => Value::Vector4(Vector4 { x: c[0], y: c[1], z: c[2], w: c[3] }),
                        _ => Value::Quaternion(Quaternion { x: c[0], y: c[1], z: c[2], w: c[3] }),
                }
        }

        fn read(&mut self, depth: i32) -> Value {
                if self.error { return Value::Null; }
                if depth > 64 || self.pos >= self.data.len() { self.error = true; return Value::Null; }
                let b = self.u8();
                if (b & 0x80) == 0 { return Value::Int(b as i64); }
                if (b & 0xE0) == 0xE0 { return Value::Int((b as i8) as i64); }
                if (b & 0xE0) == 0xA0 { let n = (b & 0x1F) as usize; return Value::String(self.read_str(n)); }
                if (b & 0xF0) == 0x90 {
                        let n = (b & 0x0F) as usize;
                        let mut arr = Vec::with_capacity(n);
                        for _ in 0..n { if self.error { break; } arr.push(self.read(depth + 1)); }
                        return Value::Array(arr);
                }
                if (b & 0xF0) == 0x80 { return self.read_map((b & 0x0F) as u32, depth); }
                match b {
                        0xC0 => Value::Null,
                        0xC2 => Value::Bool(false),
                        0xC3 => Value::Bool(true),
                        0xC4 => { let n = self.u8() as usize; Value::Binary(self.bytes(n).to_vec()) }
                        0xC5 => { let n = self.u16() as usize; Value::Binary(self.bytes(n).to_vec()) }
                        0xC6 => { let n = self.u32() as usize; Value::Binary(self.bytes(n).to_vec()) }
                        0xD9 => { let n = self.u8() as usize; Value::String(self.read_str(n)) }
                        0xDA => { let n = self.u16() as usize; Value::String(self.read_str(n)) }
                        0xDB => { let n = self.u32() as usize; Value::String(self.read_str(n)) }
                        0xCC => Value::Int(self.u8() as i64),
                        0xCD => Value::Int(self.u16() as i64),
                        0xCE => Value::Int(self.u32() as i64),
                        0xCF => { let n = self.u64(); if n > i64::MAX as u64 { Value::Float(n as f64) } else { Value::Int(n as i64) } }
                        0xD0 => Value::Int((self.u8() as i8) as i64),
                        0xD1 => Value::Int((self.u16() as i16) as i64),
                        0xD2 => Value::Int((self.u32() as i32) as i64),
                        0xD3 => Value::Int(self.u64() as i64),
                        0xCA => Value::Float(f32::from_bits(self.u32()) as f64),
                        0xCB => Value::Float(f64::from_bits(self.u64())),
                        0xDC => { let n = self.u16() as usize; if n > 65536 { self.error = true; return Value::Null; } let mut a = Vec::new(); for _ in 0..n { if self.error { break; } a.push(self.read(depth+1)); } Value::Array(a) }
                        0xDD => { let n = self.u32() as usize; if n > 65536 { self.error = true; return Value::Null; } let mut a = Vec::new(); for _ in 0..n { if self.error { break; } a.push(self.read(depth+1)); } Value::Array(a) }
                        0xD4 => self.read_ext(1),
                        0xD5 => self.read_ext(2),
                        0xD6 => self.read_ext(4),
                        0xD7 => self.read_ext(8),
                        0xD8 => self.read_ext(16),
                        0xC7 => { let n = self.u8() as usize; self.read_ext(n) }
                        0xC8 => { let n = self.u16() as usize; self.read_ext(n) }
                        0xC9 => { let n = self.u32() as usize; self.read_ext(n) }
                        0xDE => { let n = self.u16() as u32; self.read_map(n, depth) }
                        0xDF => { let n = self.u32(); self.read_map(n, depth) }
                        _ => Value::Null,
                }
        }
}

pub fn decode(data: &[u8]) -> Value {
        let mut r = MsgpackReader::new(data);
        let v = r.read(0);
        if r.error { Value::Null } else { v }
}

fn ensure_array(v: Value) -> Value {
        match v { Value::Array(_) => v, other => Value::Array(vec![other]) }
}
pub struct EventArgs {
        inner: Value,
}

impl EventArgs {
        pub fn new(v: Value) -> Self { Self { inner: v } }
        pub fn len(&self) -> usize { self.inner.len() }
        pub fn is_empty(&self) -> bool { self.inner.is_empty() }
        pub fn value(&self, i: usize) -> &Value { self.inner.at(i) }
        pub fn str(&self, i: usize) -> &str { self.inner.at(i).as_str() }
        pub fn int(&self, i: usize) -> i32 { self.inner.at(i).as_int() }
        pub fn i64(&self, i: usize) -> i64 { self.inner.at(i).as_i64() }
        pub fn f64(&self, i: usize) -> f64 { self.inner.at(i).as_f64() }
        pub fn f32(&self, i: usize) -> f32 { self.inner.at(i).as_f32() }
        pub fn bool(&self, i: usize) -> bool { self.inner.at(i).as_bool() }
        pub fn is_null(&self, i: usize) -> bool { self.inner.at(i).is_null() }
        pub fn func_ref(&self, i: usize) -> &str { self.inner.at(i).func_ref_str() }
        pub fn get<T: FromEventArg>(&self, i: usize) -> T { T::from_event_arg(self, i) }
}

pub trait FromEventArg: Sized {
        fn from_event_arg(args: &EventArgs, i: usize) -> Self;
}

impl FromEventArg for String { fn from_event_arg(args: &EventArgs, i: usize) -> Self { args.str(i).to_string() } }
impl FromEventArg for i32 { fn from_event_arg(args: &EventArgs, i: usize) -> Self { args.int(i) } }
impl FromEventArg for i64 { fn from_event_arg(args: &EventArgs, i: usize) -> Self { args.i64(i) } }
impl FromEventArg for f64 { fn from_event_arg(args: &EventArgs, i: usize) -> Self { args.f64(i) } }
impl FromEventArg for f32 { fn from_event_arg(args: &EventArgs, i: usize) -> Self { args.f32(i) } }
impl FromEventArg for bool { fn from_event_arg(args: &EventArgs, i: usize) -> Self { args.bool(i) } }
impl FromEventArg for Value { fn from_event_arg(args: &EventArgs, i: usize) -> Self { args.value(i).clone() } }

type EventHandlerFn = Rc<dyn Fn(i32, &EventArgs)>;

pub struct RawEventHandler;

pub trait IntoEventHandler<M> {
        fn into_event_handler(self, event: &str) -> EventHandlerFn;
}

impl<F: Fn(i32, &EventArgs) + 'static> IntoEventHandler<RawEventHandler> for F {
        fn into_event_handler(self, _event: &str) -> EventHandlerFn { Rc::new(self) }
}

fn report_arg_error(event: &str, error: ArgError) {
        trace_str(&format!("event '{}' argument {}: {}, handler skipped\n", event, error.index + 1, error.reason));
}

type ExportHandlerFn = Rc<dyn Fn(&EventArgs) -> Value>;

pub struct RawExportHandler;

pub trait IntoExportHandler<M> {
        fn into_export_handler(self, name: &str) -> ExportHandlerFn;
}

impl<F: Fn(&EventArgs) -> Value + 'static> IntoExportHandler<RawExportHandler> for F {
        fn into_export_handler(self, _name: &str) -> ExportHandlerFn { Rc::new(self) }
}

macro_rules! impl_typed_export_handler {
        ($($t:ident $i:tt),*) => {
                impl<F, R: Into<Value>, $($t: FromArg),*> IntoExportHandler<(R, $($t,)*)> for F where F: Fn($($t),*) -> R + 'static {
                        fn into_export_handler(self, name: &str) -> ExportHandlerFn {
                                let name = name.to_string();
                                Rc::new(move |_args: &EventArgs| -> Value {
                                        let decoded = (|| -> Result<($($t,)*), ArgError> { Ok(($(decode_arg::<$t>(_args, $i)?,)*)) })();
                                        match decoded {
                                                Ok(_values) => self($(_values.$i),*).into(),
                                                Err(error) => {
                                                        trace_str(&format!("{} argument {}: {}, call skipped\n", name, error.index + 1, error.reason));
                                                        Value::Null
                                                }
                                        }
                                })
                        }
                }
        };
}
impl_typed_export_handler!();
impl_typed_export_handler!(A 0);
impl_typed_export_handler!(A 0, B 1);
impl_typed_export_handler!(A 0, B 1, C 2);
impl_typed_export_handler!(A 0, B 1, C 2, D 3);
impl_typed_export_handler!(A 0, B 1, C 2, D 3, E 4);
impl_typed_export_handler!(A 0, B 1, C 2, D 3, E 4, F5 5);
impl_typed_export_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6);
impl_typed_export_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6, H 7);

pub struct AsyncExportHandler;

macro_rules! impl_async_export_handler {
        ($($t:ident $i:tt),*) => {
                impl<F, Fut, $($t: FromArg),*> IntoExportHandler<(AsyncExportHandler, Fut, $($t,)*)> for F
                where F: Fn($($t),*) -> Fut + 'static, Fut: Future + 'static, Fut::Output: Into<Value> {
                        fn into_export_handler(self, name: &str) -> ExportHandlerFn {
                                let name = name.to_string();
                                Rc::new(move |_args: &EventArgs| -> Value {
                                        let decoded = (|| -> Result<($($t,)*), ArgError> { Ok(($(decode_arg::<$t>(_args, $i)?,)*)) })();
                                        match decoded {
                                                Ok(_values) => start_async_export(self($(_values.$i),*)),
                                                Err(error) => {
                                                        trace_str(&format!("{} argument {}: {}, call skipped\n", name, error.index + 1, error.reason));
                                                        Value::Null
                                                }
                                        }
                                })
                        }
                }
        };
}
impl_async_export_handler!();
impl_async_export_handler!(A 0);
impl_async_export_handler!(A 0, B 1);
impl_async_export_handler!(A 0, B 1, C 2);
impl_async_export_handler!(A 0, B 1, C 2, D 3);
impl_async_export_handler!(A 0, B 1, C 2, D 3, E 4);
impl_async_export_handler!(A 0, B 1, C 2, D 3, E 4, F5 5);
impl_async_export_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6);
impl_async_export_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6, H 7);

fn deliver_async_export(callback: &str, value: Value) {
        let payload = encode(&Value::Array(vec![Value::Array(vec![value]), Value::Null]));
        invoke_function_reference(callback, &payload);
}

fn start_async_export<Fut>(future: Fut) -> Value
where Fut: Future + 'static, Fut::Output: Into<Value> {
        struct Reply {
                callback: Option<FuncRef>,
                result: Option<Value>,
        }
        let reply = Rc::new(std::cell::RefCell::new(Reply { callback: None, result: None }));
        let asked = reply.clone();
        let (retval, _) = create_canonical_ref(Rc::new(move |data: &[u8]| -> Vec<u8> {
                let decoded = ensure_array(decode(data));
                let callback = match decoded.at(0) { Value::FuncRef(r) => r.clone(), _ => return vec![0x90] };
                let ready = asked.borrow_mut().result.take();
                match ready {
                        Some(value) => deliver_async_export(&callback, value),
                        None => asked.borrow_mut().callback = Some(callback),
                }
                vec![0x90]
        }));
        if retval.is_empty() { return Value::Null; }
        create_coroutine_now(async move {
                let value: Value = future.await.into();
                let callback = reply.borrow_mut().callback.take();
                match callback {
                        Some(callback) => deliver_async_export(&callback, value),
                        None => reply.borrow_mut().result = Some(value),
                }
        });
        Value::Object(vec![("__cfx_async_retval".into(), Value::FuncRef(FuncRef::owned(retval)))])
}

macro_rules! impl_typed_event_handler {
        ($($t:ident $i:tt),*) => {
                impl<F, $($t: FromArg),*> IntoEventHandler<($($t,)*)> for F where F: Fn(i32 $(, $t)*) + 'static {
                        fn into_event_handler(self, event: &str) -> EventHandlerFn {
                                let event = event.to_string();
                                Rc::new(move |source: i32, _args: &EventArgs| {
                                        let decoded = (|| -> Result<($($t,)*), ArgError> { Ok(($(decode_arg::<$t>(_args, $i)?,)*)) })();
                                        match decoded {
                                                Ok(_values) => self(source $(, _values.$i)*),
                                                Err(error) => report_arg_error(&event, error),
                                        }
                                })
                        }
                }
        };
}
impl_typed_event_handler!();
impl_typed_event_handler!(A 0);
impl_typed_event_handler!(A 0, B 1);
impl_typed_event_handler!(A 0, B 1, C 2);
impl_typed_event_handler!(A 0, B 1, C 2, D 3);
impl_typed_event_handler!(A 0, B 1, C 2, D 3, E 4);
impl_typed_event_handler!(A 0, B 1, C 2, D 3, E 4, F5 5);
impl_typed_event_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6);
impl_typed_event_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6, H 7);
impl_typed_event_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6, H 7, I 8);
impl_typed_event_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6, H 7, I 8, J 9);
impl_typed_event_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6, H 7, I 8, J 9, K 10);
impl_typed_event_handler!(A 0, B 1, C 2, D 3, E 4, F5 5, G 6, H 7, I 8, J 9, K 10, L 11);
type RefCallbackFn = Rc<dyn Fn(&[u8]) -> Vec<u8>>;

struct EventHandlerEntry {
        id: i32,
        handler: EventHandlerFn,
}

struct TimerEntry {
        id: i32,
        next_fire: u64,
        interval_ms: u32,
        repeat: bool,
        callback: Rc<dyn Fn()>,
}

struct CoroutineEntry {
        future: Pin<Box<dyn Future<Output = ()>>>,
        resume_at: u64,
}

struct Context {
        events: HashMap<String, Vec<EventHandlerEntry>>,
        handler_event_map: HashMap<i32, String>,
        next_event_handler_id: i32,
        ticks: HashMap<i32, Rc<dyn Fn()>>,
        next_tick_id: i32,
        stops: HashMap<i32, Rc<dyn Fn()>>,
        next_stop_id: i32,
        net_safe_events: HashSet<String>,
        timers: HashMap<i32, TimerEntry>,
        next_timer_id: i32,
        coroutines: HashMap<i32, CoroutineEntry>,
        next_coroutine_id: i32,
        earliest_resume: u64,
        commands: HashMap<String, Vec<Rc<dyn Fn(i32, &CommandArgs)>>>,
        ref_callbacks: HashMap<i32, RefCallbackFn>,
        ref_counts: HashMap<i32, i32>,
        pending_ref_result: Option<(i32, Vec<u8>)>,
        next_callback_id: i32,
        state_bag_handler_refs: HashMap<i32, i32>,
        export_ref_cache: HashMap<String, FuncRef>,
        http_callbacks: HashMap<i32, Box<dyn FnOnce(HttpResponse)>>,
        http_response_registered: bool,
}

impl Context {
        fn new() -> Self {
                Self {
                        events: HashMap::new(),
                        handler_event_map: HashMap::new(),
                        next_event_handler_id: 1,
                        ticks: HashMap::new(),
                        next_tick_id: 1,
                        stops: HashMap::new(),
                        next_stop_id: 1,
                        net_safe_events: HashSet::new(),
                        timers: HashMap::new(),
                        next_timer_id: 1,
                        coroutines: HashMap::new(),
                        next_coroutine_id: 1,
                        earliest_resume: u64::MAX,
                        commands: HashMap::new(),
                        ref_callbacks: HashMap::new(),
                        ref_counts: HashMap::new(),
                        pending_ref_result: None,
                        next_callback_id: 1,
                        state_bag_handler_refs: HashMap::new(),
                        export_ref_cache: HashMap::new(),
                        http_callbacks: HashMap::new(),
                        http_response_registered: false,
                }
        }

        fn now_ms(&self) -> u64 {
                use std::time::Instant;
                static EPOCH: GlobalCell<Option<Instant>> = GlobalCell::new(None);
                let epoch = unsafe { (*EPOCH.get()).get_or_insert_with(Instant::now) };
                epoch.elapsed().as_millis() as u64
        }

        fn add_timer(&mut self, ms: u32, repeat: bool, cb: Rc<dyn Fn()>) -> i32 {
                if self.timers.len() >= 8192 {
                        trace_str("timer limit (8192) reached, dropping new timer\n");
                        return -1;
                }
                let id = allocate_id(&mut self.next_timer_id, &self.timers);
                if id < 0 { return -1; }
                let fire = self.now_ms() + ms as u64;
                self.timers.insert(id, TimerEntry { id, next_fire: fire, interval_ms: ms, repeat, callback: cb });
                id
        }
}

fn noop_waker() -> Waker {
        fn noop(_: *const ()) {}
        fn clone(p: *const ()) -> RawWaker { RawWaker::new(p, &VTABLE) }
        const VTABLE: RawWakerVTable = RawWakerVTable::new(clone, noop, noop, noop);
        unsafe { Waker::from_raw(RawWaker::new(std::ptr::null(), &VTABLE)) }
}

fn resume_coroutine_by_id(id: i32) {
        let c = ctx();
        let mut entry = match unsafe { (*c).coroutines.remove(&id) } {
                Some(e) => e,
                None => return,
        };
        let outer_wait_ms = unsafe { std::mem::replace(&mut *CORO_WAIT_MS.get(), 0) };
        let waker = noop_waker();
        let mut cx = core::task::Context::from_waker(&waker);
        let poll = entry.future.as_mut().poll(&mut cx);
        let wait_ms = unsafe { std::mem::replace(&mut *CORO_WAIT_MS.get(), outer_wait_ms) };
        match poll {
                Poll::Ready(()) => {}
                Poll::Pending => {
                        let now = unsafe { (*c).now_ms() };
                        entry.resume_at = if wait_ms > 0 { now + wait_ms as u64 } else { now };
                        unsafe { (*c).earliest_resume = (*c).earliest_resume.min(entry.resume_at) };
                        let deadline = if wait_ms <= 0 { 0 } else if wait_ms > i32::MAX as i64 { i32::MAX } else { wait_ms as i32 };
                        unsafe { __cfx_schedule_bookmark(id, deadline); }
                        unsafe { (*c).coroutines.insert(id, entry) };
                }
        }
}

fn create_ref(cb: RefCallbackFn) -> i32 {
        let c = ctx();
        let id = unsafe { allocate_id(&mut (*c).next_callback_id, &(*c).ref_callbacks) };
        if id < 0 { return -1; }
        unsafe { (*c).ref_callbacks.insert(id, cb) };
        unsafe { (*c).ref_counts.insert(id, 1) };
        let ref_idx = unsafe { __cfx_create_ref(id) };
        if ref_idx < 0 {
                unsafe { (*c).ref_callbacks.remove(&id) };
                unsafe { (*c).ref_counts.remove(&id) };
        }
        ref_idx
}

fn canonicalize_ref(ref_idx: i32) -> String {
        unsafe {
                let len = __cfx_canonicalize_ref(ref_idx, std::ptr::null_mut(), 0);
                if len <= 0 { return String::new(); }
                let mut buf = vec![0u8; (len + 1) as usize];
                __cfx_canonicalize_ref(ref_idx, buf.as_mut_ptr(), len + 1);
                buf.truncate(len as usize);
                String::from_utf8(buf).unwrap_or_default()
        }
}

fn remove_ref_idx(ref_idx: i32) {
        unsafe { __cfx_remove_ref(ref_idx); }
}

fn create_canonical_ref(cb: RefCallbackFn) -> (String, i32) {
        let ref_idx = create_ref(cb);
        if ref_idx < 0 { return (String::new(), -1); }
        let s = canonicalize_ref(ref_idx);
        if s.is_empty() {
                remove_ref_idx(ref_idx);
                return (String::new(), -1);
        }
        (s, ref_idx)
}

fn invoke_function_reference(ref_str: &str, args: &[u8]) -> Vec<u8> {
        unsafe {
                let mut out = [0u32; 2];
                __cfx_invoke_function_reference(ref_str.as_ptr(), ref_str.len() as u32, args.as_ptr(), args.len() as u32, &mut out);
                if out[0] == 0 || out[1] == 0 { return Vec::new(); }
                let data = std::slice::from_raw_parts(out[0] as *const u8, out[1] as usize).to_vec();
                std::alloc::dealloc(out[0] as *mut u8, std::alloc::Layout::from_size_align_unchecked(out[1] as usize, 1));
                data
        }
}
pub fn invoke_native_raw(hash: u64, args: &[NativeArg], num_results: u32, result_ptr_mask: u32) -> NativeCtx {
        let mut ctx = NativeCtx::new(hash);
        ctx.num_results = num_results;
        ctx.result_ptr_mask = result_ptr_mask;
        let n = args.len().min(32);
        ctx.num_args = n as u32;
        for (i, a) in args.iter().enumerate().take(n) {
                ctx.args[i] = a.value;
                if a.is_ptr { ctx.ptr_mask |= 1 << i; }
        }
        unsafe { __cfx_invoke_native(&mut ctx as *mut NativeCtx as u32); }
        ctx
}

pub fn invoke_native_void(hash: u64, args: &[NativeArg]) {
        invoke_native_raw(hash, args, 0, 0);
}

pub fn invoke_native_int(hash: u64, args: &[NativeArg]) -> i32 {
        invoke_native_raw(hash, args, 3, 0).args[0] as i32
}

pub fn invoke_native_i64(hash: u64, args: &[NativeArg]) -> i64 {
        invoke_native_raw(hash, args, 3, 0).args[0] as i64
}

pub fn invoke_native_float(hash: u64, args: &[NativeArg]) -> f32 {
        let raw = invoke_native_raw(hash, args, 3, 0).args[0] as u32;
        f32::from_bits(raw)
}

pub fn invoke_native_bool(hash: u64, args: &[NativeArg]) -> bool {
        invoke_native_raw(hash, args, 3, 0).args[0] != 0
}

pub fn invoke_native_string(hash: u64, args: &[NativeArg]) -> String {
        let ctx = invoke_native_raw(hash, args, 3, 0x1);
        get_string_result(&ctx, 0)
}

pub fn invoke_native_object(hash: u64, args: &[NativeArg]) -> Value {
        let ctx = invoke_native_raw(hash, args, 3, 0x1);
        let data = get_binary_result(&ctx, 0, 1);
        if data.is_empty() { Value::Null } else { decode(&data) }
}

pub fn invoke_native_entities(hash: u64, args: &[NativeArg]) -> Vec<i32> {
        match invoke_native_object(hash, args) {
                Value::Array(list) => list.iter().map(Value::as_int).collect(),
                _ => Vec::new(),
        }
}

pub fn invoke_native_vector3(hash: u64, args: &[NativeArg]) -> Vector3 {
        let ctx = invoke_native_raw(hash, args, 3, 0);
        Vector3 {
                x: f32::from_bits(ctx.args[0] as u32),
                y: f32::from_bits(ctx.args[1] as u32),
                z: f32::from_bits(ctx.args[2] as u32),
        }
}

pub fn get_string_result(ctx: &NativeCtx, result_idx: i32) -> String {
        unsafe {
                let len = __cfx_copy_string_result(ctx as *const NativeCtx as u32, result_idx, std::ptr::null_mut(), 0);
                if len <= 0 { return String::new(); }
                let mut buf = vec![0u8; (len + 1) as usize];
                __cfx_copy_string_result(ctx as *const NativeCtx as u32, result_idx, buf.as_mut_ptr(), len + 1);
                buf.truncate(len as usize);
                String::from_utf8(buf).unwrap_or_default()
        }
}

pub fn last_native_error() -> String {
        unsafe {
                let len = __cfx_get_last_native_error(std::ptr::null_mut(), 0);
                if len <= 0 { return String::new(); }
                let mut buf = vec![0u8; (len + 1) as usize];
                __cfx_get_last_native_error(buf.as_mut_ptr(), len + 1);
                buf.truncate(len as usize);
                String::from_utf8_lossy(&buf).into_owned()
        }
}

pub fn get_binary_result(ctx: &NativeCtx, ptr_idx: i32, size_idx: i32) -> Vec<u8> {
        unsafe {
                let len = __cfx_copy_binary_result(ctx as *const NativeCtx as u32, ptr_idx, size_idx, std::ptr::null_mut(), 0);
                if len <= 0 { return Vec::new(); }
                let mut buf = vec![0u8; len as usize];
                __cfx_copy_binary_result(ctx as *const NativeCtx as u32, ptr_idx, size_idx, buf.as_mut_ptr(), len);
                buf
        }
}
pub fn trace_str(msg: &str) {
        if !msg.is_empty() {
                unsafe { __cfx_trace(msg.as_ptr(), msg.len() as u32); }
        }
}

pub fn on<M>(event: &str, handler: impl IntoEventHandler<M>) -> i32 {
        let c = ctx();
        let first = unsafe { (*c).events.get(event).map_or(true, |v| v.is_empty()) };
        let token = unsafe { allocate_id(&mut (*c).next_event_handler_id, &(*c).handler_event_map) };
        if token < 0 { return -1; }
        unsafe { (*c).events.entry(event.to_string()).or_default() }.push(EventHandlerEntry {
                id: token,
                handler: handler.into_event_handler(event),
        });
        unsafe { (*c).handler_event_map.insert(token, event.to_string()) };
        if first {
                let mut nctx = NativeCtx::new(hash_string(b"REGISTER_RESOURCE_AS_EVENT_HANDLER"));
                nctx.num_args = 1;
                let cs = CString::new(event).unwrap_or_default();
                nctx.args[0] = cs.as_ptr() as u64;
                nctx.ptr_mask = 1;
                unsafe { __cfx_invoke_native(&mut nctx as *mut NativeCtx as u32); }
        }
        token
}

pub fn on_net<M>(event: &str, handler: impl IntoEventHandler<M>) -> i32 {
        unsafe { (*ctx()).net_safe_events.insert(event.to_string()) };
        on(event, handler)
}

pub fn register_net_event(event: &str) {
        unsafe { (*ctx()).net_safe_events.insert(event.to_string()) };
}

pub fn remove_event_handler(token: i32) {
        let c = ctx();
        let event = match unsafe { (*c).handler_event_map.remove(&token) } { Some(e) => e, None => return };
        if let Some(vec) = unsafe { (*c).events.get_mut(&event) } {
                vec.retain(|e| e.id != token);
        }
}

pub fn emit(event: &str, args: impl IntoArgs) -> bool {
        let buf = encode_args(&args.into_args());
        unsafe {
                __cfx_emit_event(event.as_ptr(), event.len() as u32, buf.as_ptr(), buf.len() as u32);
        }
        invoke_native_bool(hash_string(b"WAS_EVENT_CANCELED"), &[])
}

pub fn emit_net(event: &str, target: i32, args: impl IntoArgs) {
        let buf = encode_args(&args.into_args());
        unsafe {
                __cfx_emit_net_event(event.as_ptr(), event.len() as u32, target, buf.as_ptr(), buf.len() as u32);
        }
}

pub fn emit_latent(event: &str, target: i32, bps: i32, args: impl IntoArgs) {
        let buf = encode_args(&args.into_args());
        let event_cs = CString::new(event).unwrap_or_default();
        let target_cs = CString::new(target.to_string()).unwrap_or_default();
        invoke_native_void(hash_string(b"TRIGGER_LATENT_CLIENT_EVENT_INTERNAL"), &[NativeArg::ptr(event_cs.as_ptr() as u64), NativeArg::ptr(target_cs.as_ptr() as u64), NativeArg::ptr(buf.as_ptr() as u64), NativeArg::val(buf.len() as u32 as u64), NativeArg::val(bps as u32 as u64)]);
}

pub fn cancel_event() {
        unsafe { __cfx_cancel_event(); }
}

pub fn was_event_canceled() -> bool {
        unsafe { __cfx_was_event_canceled() != 0 }
}

pub fn on_tick(handler: impl Fn() + 'static) -> i32 {
        let c = ctx();
        let id = unsafe { allocate_id(&mut (*c).next_tick_id, &(*c).ticks) };
        if id < 0 { return -1; }
        unsafe { (*c).ticks.insert(id, Rc::new(handler)) };
        id
}

pub fn remove_tick(id: i32) {
        unsafe { (*ctx()).ticks.remove(&id) };
}

pub fn on_stop(handler: impl Fn() + 'static) -> i32 {
        let c = ctx();
        let id = unsafe { allocate_id(&mut (*c).next_stop_id, &(*c).stops) };
        if id < 0 { return -1; }
        unsafe { (*c).stops.insert(id, Rc::new(handler)) };
        id
}

pub fn remove_stop(id: i32) {
        unsafe { (*ctx()).stops.remove(&id) };
}

pub fn set_timeout(ms: u32, cb: impl Fn() + 'static) -> i32 {
        unsafe { (*ctx()).add_timer(ms, false, Rc::new(cb)) }
}

pub fn set_interval(ms: u32, cb: impl Fn() + 'static) -> i32 {
        unsafe { (*ctx()).add_timer(ms, true, Rc::new(cb)) }
}

pub fn clear_timer(id: i32) {
        unsafe { (*ctx()).timers.remove(&id) };
}

pub fn clear_timeout(id: i32) { clear_timer(id); }
pub fn clear_interval(id: i32) { clear_timer(id); }

pub fn get_resource_metadata(key: &str, index: i32) -> String {
        unsafe {
                let len = __cfx_get_resource_metadata(key.as_ptr(), key.len() as u32, index, std::ptr::null_mut(), 0);
                if len <= 0 { return String::new(); }
                let mut buf = vec![0u8; (len + 1) as usize];
                __cfx_get_resource_metadata(key.as_ptr(), key.len() as u32, index, buf.as_mut_ptr(), len + 1);
                buf.truncate(len as usize);
                String::from_utf8(buf).unwrap_or_default()
        }
}

pub fn get_num_resource_metadata(key: &str) -> i32 {
        unsafe { __cfx_get_num_resource_metadata(key.as_ptr(), key.len() as u32) }
}

pub fn is_manifest_version_v2_between(lower: &str, upper: &str) -> bool {
        unsafe {
                __cfx_is_manifest_version_v2_between(lower.as_ptr(), lower.len() as u32, upper.as_ptr(), upper.len() as u32) != 0
        }
}

pub fn get_current_resource_name() -> String {
        let ctx = invoke_native_raw(hash_string(b"GET_CURRENT_RESOURCE_NAME"), &[], 1, 0x1);
        get_string_result(&ctx, 0)
}

pub fn get_invoking_resource() -> String {
        let ctx = invoke_native_raw(hash_string(b"GET_INVOKING_RESOURCE"), &[], 1, 0x1);
        get_string_result(&ctx, 0)
}

pub fn get_resource_path(resource: &str) -> String {
        let cs = CString::new(resource).unwrap_or_default();
        let ctx = invoke_native_raw(hash_string(b"GET_RESOURCE_PATH"), &[NativeArg::ptr(cs.as_ptr() as u64)], 1, 0x1);
        get_string_result(&ctx, 0)
}

pub fn get_convar(name: &str, default: &str) -> String {
        let n = CString::new(name).unwrap_or_default();
        let d = CString::new(default).unwrap_or_default();
        let ctx = invoke_native_raw(hash_string(b"GET_CONVAR"), &[NativeArg::ptr(n.as_ptr() as u64), NativeArg::ptr(d.as_ptr() as u64)], 1, 0x1);
        get_string_result(&ctx, 0)
}

pub fn get_convar_int(name: &str, default: i32) -> i32 {
        let n = CString::new(name).unwrap_or_default();
        let ctx = invoke_native_raw(hash_string(b"GET_CONVAR_INT"), &[NativeArg::ptr(n.as_ptr() as u64), NativeArg::val(default as u32 as u64)], 1, 0);
        ctx.args[0] as i32
}

pub fn set_convar(name: &str, value: &str) {
        let n = CString::new(name).unwrap_or_default();
        let v = CString::new(value).unwrap_or_default();
        invoke_native_void(hash_string(b"SET_CONVAR"), &[NativeArg::ptr(n.as_ptr() as u64), NativeArg::ptr(v.as_ptr() as u64)]);
}

pub fn set_convar_replicated(name: &str, value: &str) {
        let n = CString::new(name).unwrap_or_default();
        let v = CString::new(value).unwrap_or_default();
        invoke_native_void(hash_string(b"SET_CONVAR_REPLICATED"), &[NativeArg::ptr(n.as_ptr() as u64), NativeArg::ptr(v.as_ptr() as u64)]);
}

pub fn get_resource_state(resource: &str) -> String {
        let cs = CString::new(resource).unwrap_or_default();
        let ctx = invoke_native_raw(hash_string(b"GET_RESOURCE_STATE"), &[NativeArg::ptr(cs.as_ptr() as u64)], 1, 0x1);
        get_string_result(&ctx, 0)
}

pub fn get_num_resources() -> i32 {
        invoke_native_int(hash_string(b"GET_NUM_RESOURCES"), &[])
}

pub fn get_resource_by_index(index: i32) -> String {
        let ctx = invoke_native_raw(hash_string(b"GET_RESOURCE_BY_FIND_INDEX"), &[NativeArg::val(index as u32 as u64)], 1, 0x1);
        get_string_result(&ctx, 0)
}

pub fn get_resources() -> Vec<String> {
        let num = get_num_resources();
        let mut result = Vec::new();
        for i in 0..num {
                let name = get_resource_by_index(i);
                if !name.is_empty() { result.push(name); }
        }
        result
}

pub fn on_resource_start(handler: impl Fn(&str) + 'static) -> i32 {
        on("onResourceStart", move |_source: i32, args: &EventArgs| {
                if args.len() > 0 { handler(args.str(0)); }
        })
}

pub fn on_resource_stop(handler: impl Fn(&str) + 'static) -> i32 {
        on("onResourceStop", move |_source: i32, args: &EventArgs| {
                if args.len() > 0 { handler(args.str(0)); }
        })
}

fn create_handler_ref(handler: ExportHandlerFn) -> String {
        let (r, _) = create_canonical_ref(Rc::new(move |args_data: &[u8]| -> Vec<u8> {
                let ea = EventArgs::new(ensure_array(decode(args_data)));
                encode(&Value::Array(vec![handler(&ea)]))
        }));
        r
}

pub fn callback<M>(handler: impl IntoExportHandler<M>) -> Value {
        let r = create_handler_ref(handler.into_export_handler("callback"));
        if r.is_empty() { Value::Null } else { Value::FuncRef(FuncRef::owned(r)) }
}

pub fn add_export<M>(name: &str, handler: impl IntoExportHandler<M>) {
        let export_ref = create_handler_ref(handler.into_export_handler(&format!("export '{}'", name)));
        if export_ref.is_empty() { return; }
        let res_name = get_current_resource_name();
        let event_name = format!("__cfx_export_{}_{}", res_name, name);
        let export_ref = FuncRef::new(export_ref);
        on(&event_name, move |_source: i32, args: &EventArgs| {
                if args.len() == 0 { return; }
                let setter_ref = args.func_ref(0);
                if setter_ref.is_empty() { return; }
                let payload = encode(&Value::Array(vec![Value::FuncRef(export_ref.clone())]));
                invoke_function_reference(setter_ref, &payload);
        });
}

pub fn call_export(resource: &str, name: &str, args: impl IntoArgs) -> Value {
        let args = args.into_args();
        let args = args.as_slice();
        let cache_key = format!("{}/{}", resource, name);
        let c = ctx();
        let cached = unsafe { (*c).export_ref_cache.get(&cache_key).cloned() };
        let mut export_ref = match cached {
                Some(cached) => cached,
                None => {
                        let resolved = match resolve_export_ref(resource, name) { Some(r) => r, None => return Value::Null };
                        unsafe { (*ctx()).export_ref_cache.insert(cache_key.clone(), resolved.clone()) };
                        resolved
                }
        };
        let user_payload = encode(&Value::Array(args.to_vec()));
        let ret_data = invoke_function_reference(&export_ref, &user_payload);
        if ret_data.is_empty() {
                unsafe { (*ctx()).export_ref_cache.remove(&cache_key) };
                let resolved = match resolve_export_ref(resource, name) { Some(r) => r, None => return Value::Null };
                unsafe { (*ctx()).export_ref_cache.insert(cache_key.clone(), resolved.clone()) };
                if resolved.as_str() == export_ref.as_str() {
                        return Value::Null;
                }
                export_ref = resolved;
                let ret_data = invoke_function_reference(&export_ref, &user_payload);
                if ret_data.is_empty() {
                        unsafe { (*ctx()).export_ref_cache.remove(&cache_key) };
                        return Value::Null;
                }
                let result = decode(&ret_data);
                return match result { Value::Array(ref a) if !a.is_empty() => a[0].clone(), other => other };
        }
        let result = decode(&ret_data);
        match result { Value::Array(ref a) if !a.is_empty() => a[0].clone(), other => other }
}

pub struct ExportCall {
        result: Rc<std::cell::RefCell<Option<Value>>>,
}

impl ExportCall {
        pub fn then(self, callback: impl FnOnce(Value) + 'static) {
                create_coroutine_now(async move { callback(self.await) });
        }
}

impl Future for ExportCall {
        type Output = Value;
        fn poll(self: Pin<&mut Self>, _cx: &mut std::task::Context<'_>) -> Poll<Value> {
                match self.result.borrow_mut().take() {
                        Some(value) => Poll::Ready(value),
                        None => Poll::Pending,
                }
        }
}

pub fn call_export_async(resource: &str, name: &str, args: impl IntoArgs) -> ExportCall {
        let result = Rc::new(std::cell::RefCell::new(None));
        let slot = result.clone();
        start_export_call(resource, name, &args.into_args(), move |value| *slot.borrow_mut() = Some(value));
        ExportCall { result }
}

fn start_export_call(resource: &str, name: &str, args: &[Value], callback: impl FnOnce(Value) + 'static) {
        let result = call_export(resource, name, args);
        if let Value::Object(ref entries) = result {
                if let Some(async_retval) = entries.iter().find(|(k, _)| k.as_str() == "__cfx_async_retval").map(|(_, v)| v) {
                        if let Value::FuncRef(ref async_ref) = async_retval {
                                let async_ref = async_ref.clone();
                                let shared_cb = std::rc::Rc::new(std::cell::RefCell::new(Some(callback)));
                                let shared_cb2 = shared_cb.clone();
                                let export_name = format!("{}:{}", resource, name);
                                let (cb_ref_str, _cb_ref_idx) = create_canonical_ref(Rc::new(move |data: &[u8]| -> Vec<u8> {
                                        let decoded = ensure_array(decode(data));
                                        let error = decoded.at(1);
                                        let failed = match error {
                                                Value::Null | Value::Bool(false) => false,
                                                Value::String(s) => !s.is_empty(),
                                                _ => true,
                                        };
                                        let val = if failed {
                                                let message = match error { Value::String(s) => s.clone(), other => json::stringify(other) };
                                                trace_str(&format!("export '{}' failed: {}\n", export_name, message));
                                                Value::Null
                                        } else {
                                                match decoded.at(0) {
                                                        Value::Array(a) => a.first().cloned().unwrap_or(Value::Null),
                                                        other => other.clone(),
                                                }
                                        };
                                        if let Some(cb) = shared_cb2.borrow_mut().take() { cb(val); }
                                        vec![0x90]
                                }));
                                if cb_ref_str.is_empty() {
                                        if let Some(cb) = shared_cb.borrow_mut().take() { cb(Value::Null); }
                                        return;
                                }
                                let payload = encode(&Value::Array(vec![Value::FuncRef(FuncRef::owned(cb_ref_str))]));
                                invoke_function_reference(&async_ref, &payload);
                                return;
                        }
                }
        }
        callback(result);
}

fn resolve_export_ref(resource: &str, name: &str) -> Option<FuncRef> {
        let captured: Rc<std::cell::RefCell<Option<FuncRef>>> = Rc::new(std::cell::RefCell::new(None));
        let captured2 = captured.clone();
        let (setter_ref_str, setter_ref_idx) = create_canonical_ref(Rc::new(move |data: &[u8]| -> Vec<u8> {
                let r = match decode(data) {
                        Value::FuncRef(r) => Some(r),
                        Value::Array(mut a) if !a.is_empty() => match a.swap_remove(0) { Value::FuncRef(r) => Some(r), _ => None },
                        _ => None,
                };
                *captured2.borrow_mut() = r;
                vec![0x90]
        }));
        if setter_ref_str.is_empty() { return None; }
        let setter_payload = encode(&Value::Array(vec![Value::FuncRef(FuncRef::new(setter_ref_str))]));
        let event_name = format!("__cfx_export_{}_{}", resource, name);
        unsafe {
                __cfx_emit_event(event_name.as_ptr(), event_name.len() as u32, setter_payload.as_ptr(), setter_payload.len() as u32);
        }
        remove_ref_idx(setter_ref_idx);
        let result = captured.borrow_mut().take();
        result
}

pub struct CommandArgs {
        args: Vec<String>,
        pub raw: String,
}

impl std::ops::Deref for CommandArgs {
        type Target = [String];
        fn deref(&self) -> &[String] { &self.args }
}

pub fn on_command(command: &str, handler: impl Fn(i32, &CommandArgs) + 'static, restricted: bool) {
        let cmd = command.to_string();
        let c = ctx();
        let handlers = unsafe { (*c).commands.entry(cmd.clone()).or_default() };
        handlers.push(Rc::new(handler));
        if handlers.len() > 1 { return; }
        let cmd2 = cmd.clone();
        let (cb_ref, _) = create_canonical_ref(Rc::new(move |args_data: &[u8]| -> Vec<u8> {
                let decoded = ensure_array(decode(args_data));
                let source = if decoded.len() > 0 { decoded.at(0).as_int() } else { 0 };
                let args = match decoded.at(1) {
                        Value::Array(arr) => arr.iter().map(|v| v.as_str().to_string()).collect(),
                        _ => Vec::new(),
                };
                let cmd_args = CommandArgs { args, raw: decoded.at(2).as_str().to_string() };
                let c = ctx();
                let snapshot: Vec<Rc<dyn Fn(i32, &CommandArgs)>> = unsafe { (*c).commands.get(&cmd2) }
                        .map(|v| v.iter().map(Rc::clone).collect())
                        .unwrap_or_default();
                for h in snapshot { h(source, &cmd_args); }
                vec![0x90]
        }));
        if cb_ref.is_empty() { return; }
        let cmd_cs = CString::new(command).unwrap_or_default();
        let ref_cs = CString::new(cb_ref).unwrap_or_default();
        invoke_native_void(hash_string(b"REGISTER_COMMAND"), &[NativeArg::ptr(cmd_cs.as_ptr() as u64), NativeArg::ptr(ref_cs.as_ptr() as u64), NativeArg::val(restricted as u64)]);
}

pub fn add_state_bag_change_handler(key_filter: &str, bag_filter: &str, handler: impl Fn(&str, &str, &Value, i32, bool) + 'static) -> i32 {
        let handler = std::rc::Rc::new(handler);
        let (cb_ref, ref_idx) = create_canonical_ref(Rc::new(move |args_data: &[u8]| -> Vec<u8> {
                let decoded = ensure_array(decode(args_data));
                if decoded.len() >= 5 {
                        let bag = decoded.at(0).as_str();
                        let key = decoded.at(1).as_str();
                        let value = decoded.at(2);
                        let source = decoded.at(3).as_int();
                        let replicated = decoded.at(4).as_bool();
                        handler(bag, key, value, source, replicated);
                }
                vec![0x90]
        }));
        if cb_ref.is_empty() { return -1; }
        let host_ref = ref_idx;
        let key_cs = if key_filter.is_empty() { CString::default() } else { CString::new(key_filter).unwrap_or_default() };
        let bag_cs = if bag_filter.is_empty() { CString::default() } else { CString::new(bag_filter).unwrap_or_default() };
        let cb_cs = CString::new(cb_ref).unwrap_or_default();
        let r = invoke_native_raw(hash_string(b"ADD_STATE_BAG_CHANGE_HANDLER"), &[
                if key_filter.is_empty() { NativeArg::ptr(std::ptr::null::<u8>() as u64) } else { NativeArg::ptr(key_cs.as_ptr() as u64) },
                if bag_filter.is_empty() { NativeArg::ptr(std::ptr::null::<u8>() as u64) } else { NativeArg::ptr(bag_cs.as_ptr() as u64) },
                NativeArg::ptr(cb_cs.as_ptr() as u64),
        ], 1, 0);
        let cookie = r.args[0] as i32;
        unsafe { (*ctx()).state_bag_handler_refs.insert(cookie, host_ref) };
        cookie
}

pub fn remove_state_bag_change_handler(cookie: i32) {
        invoke_native_void(hash_string(b"REMOVE_STATE_BAG_CHANGE_HANDLER"), &[NativeArg::val(cookie as u32 as u64)]);
        let c = ctx();
        if let Some(host_ref) = unsafe { (*c).state_bag_handler_refs.remove(&cookie) } {
                remove_ref_idx(host_ref);
        }
}

pub fn set_state_bag_value(bag: &str, key: &str, value: impl Into<Value>, replicated: bool) {
        if let Some(net_id) = bag.strip_prefix("entity:").and_then(|id| id.parse::<i32>().ok()) {
                let entity = invoke_native_int(hash_string(b"NETWORK_GET_ENTITY_FROM_NETWORK_ID"), &[NativeArg::val(net_id as u32 as u64)]);
                ensure_entity_state_bag(entity);
        }
        let encoded = encode(&value.into());
        let bag_cs = CString::new(bag).unwrap_or_default();
        let key_cs = CString::new(key).unwrap_or_default();
        invoke_native_void(hash_string(b"SET_STATE_BAG_VALUE"), &[NativeArg::ptr(bag_cs.as_ptr() as u64), NativeArg::ptr(key_cs.as_ptr() as u64), NativeArg::ptr(encoded.as_ptr() as u64), NativeArg::val(encoded.len() as u64), NativeArg::val(if replicated { 1 } else { 0 })]);
}

pub fn get_state_bag_value(bag: &str, key: &str) -> Value {
        let bag_cs = CString::new(bag).unwrap_or_default();
        let key_cs = CString::new(key).unwrap_or_default();
        let r = invoke_native_raw(hash_string(b"GET_STATE_BAG_VALUE"), &[NativeArg::ptr(bag_cs.as_ptr() as u64), NativeArg::ptr(key_cs.as_ptr() as u64)], 2, 0x1);
        let data = get_binary_result(&r, 0, 1);
        if data.is_empty() { Value::Null } else { decode(&data) }
}

pub fn state_bag_has_key(bag: &str, key: &str) -> bool {
        let bag_cs = CString::new(bag).unwrap_or_default();
        let key_cs = CString::new(key).unwrap_or_default();
        invoke_native_bool(hash_string(b"STATE_BAG_HAS_KEY"), &[NativeArg::ptr(bag_cs.as_ptr() as u64), NativeArg::ptr(key_cs.as_ptr() as u64)])
}

pub fn get_state_bag_keys(bag: &str) -> Vec<String> {
        let bag_cs = CString::new(bag).unwrap_or_default();
        let r = invoke_native_raw(hash_string(b"GET_STATE_BAG_KEYS"), &[NativeArg::ptr(bag_cs.as_ptr() as u64)], 2, 0x1);
        let data = get_binary_result(&r, 0, 1);
        if data.is_empty() { return Vec::new(); }
        let arr = decode(&data);
        match arr {
                Value::Array(a) => a.iter().map(|v| v.as_str().to_string()).collect(),
                _ => Vec::new(),
        }
}

pub fn set_player_state(server_id: i32, key: &str, value: impl Into<Value>) {
        set_state_bag_value(&format!("player:{}", server_id), key, value, true);
}

pub fn get_player_state(server_id: i32, key: &str) -> Value {
        get_state_bag_value(&format!("player:{}", server_id), key)
}

fn ensure_entity_state_bag(entity: i32) {
        invoke_native_void(hash_string(b"ENSURE_ENTITY_STATE_BAG"), &[NativeArg::val(entity as u32 as u64)]);
}

fn entity_bag(entity: i32) -> String {
        ensure_entity_state_bag(entity);
        format!("entity:{}", invoke_native_int(hash_string(b"NETWORK_GET_NETWORK_ID_FROM_ENTITY"), &[NativeArg::val(entity as u32 as u64)]))
}

pub fn set_entity_state(entity: i32, key: &str, value: impl Into<Value>) {
        set_state_bag_value(&entity_bag(entity), key, value, true);
}

pub fn get_entity_state(entity: i32, key: &str) -> Value {
        get_state_bag_value(&entity_bag(entity), key)
}

pub fn set_global_state(key: &str, value: impl Into<Value>) {
        set_state_bag_value("global", key, value, true);
}

pub fn get_global_state(key: &str) -> Value {
        get_state_bag_value("global", key)
}

pub fn set_kvp(key: &str, value: &str) {
        let k = CString::new(key).unwrap_or_default();
        let v = CString::new(value).unwrap_or_default();
        invoke_native_void(hash_string(b"SET_RESOURCE_KVP"), &[NativeArg::ptr(k.as_ptr() as u64), NativeArg::ptr(v.as_ptr() as u64)]);
}

pub fn set_kvp_int(key: &str, value: i32) {
        let k = CString::new(key).unwrap_or_default();
        invoke_native_void(hash_string(b"SET_RESOURCE_KVP_INT"), &[NativeArg::ptr(k.as_ptr() as u64), NativeArg::val(value as u32 as u64)]);
}

pub fn set_kvp_float(key: &str, value: f32) {
        let k = CString::new(key).unwrap_or_default();
        invoke_native_void(hash_string(b"SET_RESOURCE_KVP_FLOAT"), &[NativeArg::ptr(k.as_ptr() as u64), NativeArg::val(value.to_bits() as u64)]);
}

pub fn get_kvp_string(key: &str) -> String {
        let k = CString::new(key).unwrap_or_default();
        invoke_native_string(hash_string(b"GET_RESOURCE_KVP_STRING"), &[NativeArg::ptr(k.as_ptr() as u64)])
}

pub fn get_kvp_int(key: &str) -> i32 {
        let k = CString::new(key).unwrap_or_default();
        invoke_native_int(hash_string(b"GET_RESOURCE_KVP_INT"), &[NativeArg::ptr(k.as_ptr() as u64)])
}

pub fn get_kvp_float(key: &str) -> f32 {
        let k = CString::new(key).unwrap_or_default();
        invoke_native_float(hash_string(b"GET_RESOURCE_KVP_FLOAT"), &[NativeArg::ptr(k.as_ptr() as u64)])
}

pub fn delete_kvp(key: &str) {
        let k = CString::new(key).unwrap_or_default();
        invoke_native_void(hash_string(b"DELETE_RESOURCE_KVP"), &[NativeArg::ptr(k.as_ptr() as u64)]);
}

pub fn flush_kvp() {
        invoke_native_void(hash_string(b"FLUSH_RESOURCE_KVP"), &[]);
}

pub fn find_kvp(prefix: &str) -> Vec<String> {
        let mut keys = Vec::new();
        let p = CString::new(prefix).unwrap_or_default();
        let handle = invoke_native_int(hash_string(b"START_FIND_KVP"), &[NativeArg::ptr(p.as_ptr() as u64)]);
        if handle == -1 { return keys; }
        loop {
                let key = invoke_native_string(hash_string(b"FIND_KVP"), &[NativeArg::val(handle as u32 as u64)]);
                if key.is_empty() { break; }
                keys.push(key);
        }
        invoke_native_void(hash_string(b"END_FIND_KVP"), &[NativeArg::val(handle as u32 as u64)]);
        keys
}

pub struct ProcessResult {
        pub status: i32,
        pub exit_code: i32,
        pub output: String,
        pub ok: bool,
        pub truncated: bool,
}

impl ProcessResult {
        pub fn is_running(&self) -> bool { self.status == -3 }
        pub fn is_timed_out(&self) -> bool { self.status == -4 }
        pub fn is_error(&self) -> bool { self.status == -1 }
        pub fn is_invalid(&self) -> bool { self.status == -2 }
        pub fn is_done(&self) -> bool { self.status >= 0 }
}

pub fn spawn_process(command: &str) -> i32 {
        unsafe { __cfx_spawn_process(command.as_ptr(), command.len() as u32) }
}

pub fn poll_process(process_id: i32, max_output: usize) -> ProcessResult {
        let capped = max_output.min(1 << 20);
        let mut buf = vec![0u8; capped];
        let mut info = [0i32; 2];
        let mut raw = unsafe { __cfx_poll_process(process_id, buf.as_mut_ptr(), capped as i32, &mut info) };
        if raw > 0 && raw as usize > capped && raw <= 1 << 20 {
                buf.resize(raw as usize, 0);
                raw = unsafe { __cfx_poll_process(process_id, buf.as_mut_ptr(), raw, &mut info) };
        }
        if raw < 0 {
                return ProcessResult { status: raw, exit_code: 0, output: String::new(), ok: false, truncated: false };
        }
        buf.truncate(raw as usize);
        let output = String::from_utf8_lossy(&buf).into_owned();
        ProcessResult { status: raw, exit_code: info[0], output, ok: true, truncated: info[1] != 0 }
}

pub struct WorkerResult {
        pub status: i32,
        pub output: Vec<u8>,
}

impl WorkerResult {
        pub fn is_running(&self) -> bool { self.status == -3 }
        pub fn is_error(&self) -> bool { self.status == -1 }
        pub fn is_invalid(&self) -> bool { self.status == -2 }
        pub fn is_done(&self) -> bool { self.status >= 0 }
        pub fn output_str(&self) -> &str {
                core::str::from_utf8(&self.output).unwrap_or("")
        }
}

pub fn create_worker(fn_name: &str, input: &[u8]) -> i32 {
        unsafe { __cfx_create_worker(fn_name.as_ptr(), fn_name.len() as u32, input.as_ptr(), input.len() as u32) }
}

pub fn poll_worker(worker_id: i32, max_output: usize) -> WorkerResult {
        let capped = max_output.min(1 << 20);
        let mut buf = vec![0u8; capped];
        let mut raw = unsafe { __cfx_poll_worker(worker_id, buf.as_mut_ptr(), capped as i32) };
        if raw > 0 && raw as usize > capped && raw <= 1 << 20 {
                buf.resize(raw as usize, 0);
                raw = unsafe { __cfx_poll_worker(worker_id, buf.as_mut_ptr(), raw) };
        }
        if raw >= 0 {
                buf.truncate(raw as usize);
        } else {
                buf.clear();
        }
        WorkerResult { status: raw, output: buf }
}

pub fn run_worker(fn_name: &str, input: &[u8]) -> impl Future<Output = WorkerResult> {
        let id = create_worker(fn_name, input);
        core::future::poll_fn(move |_| {
                if id < 0 { return Poll::Ready(WorkerResult { status: if id == -3 { -1 } else { id }, output: Vec::new() }); }
                let result = poll_worker(id, 4096);
                if result.is_running() { Poll::Pending } else { Poll::Ready(result) }
        })
}

pub fn run_process(command: &str) -> impl Future<Output = ProcessResult> {
        let id = spawn_process(command);
        core::future::poll_fn(move |_| {
                if id < 0 { return Poll::Ready(ProcessResult { status: if id == -3 { -1 } else { id }, exit_code: 0, output: String::new(), ok: false, truncated: false }); }
                let result = poll_process(id, 4096);
                if result.is_running() { Poll::Pending } else { Poll::Ready(result) }
        })
}

pub fn wait(ms: u64) -> impl Future<Output = ()> {
        struct WaitFuture(bool, u64);
        impl Future for WaitFuture {
                type Output = ();
                fn poll(mut self: Pin<&mut Self>, _cx: &mut core::task::Context<'_>) -> Poll<()> {
                        if self.0 {
                                Poll::Ready(())
                        } else {
                                self.0 = true;
                                unsafe { *CORO_WAIT_MS.get() = self.1 as i64; }
                                Poll::Pending
                        }
                }
        }
        WaitFuture(false, ms)
}

pub fn create_coroutine(f: impl Future<Output = ()> + 'static) {
        create_coroutine_impl(f, false);
}

pub fn create_coroutine_now(f: impl Future<Output = ()> + 'static) {
        create_coroutine_impl(f, true);
}

pub fn create_thread(f: impl Future<Output = ()> + 'static) {
        create_coroutine(f);
}

fn resume_coroutine_if_due(id: i32) {
        let c = ctx();
        let due = match unsafe { (*c).coroutines.get(&id) } {
                Some(e) => (unsafe { (*c).now_ms() }) >= e.resume_at,
                None => false,
        };
        if due {
                resume_coroutine_by_id(id);
        }
}

fn has_due_coroutines() -> bool {
        let c = ctx();
        unsafe { !(*c).coroutines.is_empty() && (*c).now_ms() >= (*c).earliest_resume }
}

fn create_coroutine_impl(f: impl Future<Output = ()> + 'static, resume_now: bool) {
        let c = ctx();
        if unsafe { (*c).coroutines.len() } >= 4096 {
                trace_str("coroutine limit (4096) reached, dropping new thread\n");
                return;
        }
        let id = unsafe { allocate_id(&mut (*c).next_coroutine_id, &(*c).coroutines) };
        if id < 0 {
                trace_str("no free coroutine id, dropping new thread\n");
                return;
        }
        let now = unsafe { (*c).now_ms() };
        unsafe { (*c).coroutines.insert(id, CoroutineEntry { future: Box::pin(f), resume_at: now }) };
        unsafe { (*c).earliest_resume = (*c).earliest_resume.min(now) };
        if resume_now {
                resume_coroutine_by_id(id);
        } else {
                unsafe { __cfx_schedule_bookmark(id, 0); }
        }
}

pub struct HttpResponse {
        pub status: i32,
        pub body: String,
        pub headers: HashMap<String, String>,
        pub error: String,
}

impl HttpResponse {
        pub fn ok(&self) -> bool { (200..300).contains(&self.status) }
}

pub struct HttpRequest {
        url: String,
        method: String,
        body: String,
        headers: Vec<(String, String)>,
        follow_location: bool,
}

pub fn http_request(url: &str) -> HttpRequest {
        HttpRequest { url: url.to_string(), method: "GET".to_string(), body: String::new(), headers: Vec::new(), follow_location: true }
}

impl HttpRequest {
        pub fn method(mut self, method: &str) -> Self { self.method = method.to_string(); self }
        pub fn body(mut self, body: impl Into<String>) -> Self { self.body = body.into(); self }
        pub fn header(mut self, name: &str, value: &str) -> Self { self.headers.push((name.to_string(), value.to_string())); self }
        pub fn follow_location(mut self, follow: bool) -> Self { self.follow_location = follow; self }
        pub fn then(self, callback: impl FnOnce(HttpResponse) + 'static) { start_http_request(self, callback); }
}

pub struct HttpCall {
        response: Rc<std::cell::RefCell<Option<HttpResponse>>>,
}

impl Future for HttpCall {
        type Output = HttpResponse;
        fn poll(self: Pin<&mut Self>, _cx: &mut std::task::Context<'_>) -> Poll<HttpResponse> {
                match self.response.borrow_mut().take() {
                        Some(response) => Poll::Ready(response),
                        None => Poll::Pending,
                }
        }
}

impl std::future::IntoFuture for HttpRequest {
        type Output = HttpResponse;
        type IntoFuture = HttpCall;
        fn into_future(self) -> HttpCall {
                let response = Rc::new(std::cell::RefCell::new(None));
                let slot = response.clone();
                start_http_request(self, move |result| *slot.borrow_mut() = Some(result));
                HttpCall { response }
        }
}

fn start_http_request(request: HttpRequest, callback: impl FnOnce(HttpResponse) + 'static) {
        let headers: Vec<String> = request.headers.iter().map(|(k, v)| format!("{}:{}", json::quote(k), json::quote(v))).collect();
        let payload = format!(r#"{{"url":{},"method":{},"data":{},"headers":{{{}}},"followLocation":{}}}"#, json::quote(&request.url), json::quote(&request.method), json::quote(&request.body), headers.join(","), request.follow_location);
        let payload_cs = CString::new(payload.as_str()).unwrap_or_default();
        let r = invoke_native_raw(hash_string(b"PERFORM_HTTP_REQUEST_INTERNAL"), &[NativeArg::ptr(payload_cs.as_ptr() as u64), NativeArg::val(payload.len() as u64)], 1, 0);
        let token = r.args[0] as i32;
        if token == -1 {
                callback(HttpResponse { status: 0, body: String::new(), headers: HashMap::new(), error: "request could not be started".to_string() });
                return;
        }
        let c = ctx();
        unsafe { (*c).http_callbacks.insert(token, Box::new(callback)) };
        if unsafe { (*c).http_response_registered } { return; }
        unsafe { (*c).http_response_registered = true };
        on("__cfx_internal:httpResponse", |_source: i32, args: &EventArgs| {
                if args.len() < 4 { return; }
                let c = ctx();
                if let Some(cb) = unsafe { (*c).http_callbacks.remove(&args.int(0)) } {
                        let headers = match args.value(3) {
                                Value::Object(entries) => entries.iter().map(|(k, v)| (k.clone(), v.as_str().to_string())).collect(),
                                _ => HashMap::new(),
                        };
                        let mut response = HttpResponse { status: args.int(1), body: args.str(2).to_string(), headers, error: args.str(4).to_string() };
                        let prefix = format!("HTTP {}: ", response.status);
                        if response.body.is_empty() && response.error.starts_with(&prefix) {
                                response.body = response.error.split_off(prefix.len());
                                response.error.truncate(prefix.len() - 2);
                        }
                        cb(response);
                }
        });
}

pub mod json {
        use super::Value;

        pub fn quote(s: &str) -> String {
                let mut out = String::with_capacity(s.len() + 2);
                out.push('"');
                for c in s.chars() {
                        match c {
                                '"' => out.push_str("\\\""),
                                '\\' => out.push_str("\\\\"),
                                '\n' => out.push_str("\\n"),
                                '\r' => out.push_str("\\r"),
                                '\t' => out.push_str("\\t"),
                                c if (c as u32) < 0x20 => { out.push_str(&format!("\\u{:04x}", c as u32)); }
                                c => out.push(c),
                        }
                }
                out.push('"');
                out
        }

        const MAX_DEPTH: i32 = 128;

        struct Parser<'a> {
                src: &'a [u8],
                pos: usize,
                error: bool,
        }

        impl<'a> Parser<'a> {
                fn peek(&self) -> u8 { if self.pos < self.src.len() { self.src[self.pos] } else { 0 } }

                fn consume(&mut self) -> u8 {
                        if self.pos < self.src.len() { self.pos += 1; self.src[self.pos - 1] } else { 0 }
                }

                fn skip_ws(&mut self) {
                        while self.pos < self.src.len() && matches!(self.src[self.pos], b' ' | b'\t' | b'\n' | b'\r') { self.pos += 1; }
                }

                fn expect(&mut self, c: u8) -> bool {
                        self.skip_ws();
                        if self.consume() != c { self.error = true; return false; }
                        true
                }

                fn parse_hex4(&mut self) -> u32 {
                        let mut val = 0u32;
                        for _ in 0..4 {
                                let h = self.consume();
                                val <<= 4;
                                match h {
                                        b'0'..=b'9' => val |= (h - b'0') as u32,
                                        b'a'..=b'f' => val |= (h - b'a' + 10) as u32,
                                        b'A'..=b'F' => val |= (h - b'A' + 10) as u32,
                                        _ => self.error = true,
                                }
                        }
                        val
                }

                fn parse_string(&mut self) -> String {
                        if !self.expect(b'"') { return String::new(); }
                        let mut out: Vec<u8> = Vec::new();
                        while self.pos < self.src.len() {
                                let c = self.consume();
                                if c == b'"' { return String::from_utf8_lossy(&out).into_owned(); }
                                if c != b'\\' { out.push(c); continue; }
                                let e = self.consume();
                                match e {
                                        b'"' => out.push(b'"'),
                                        b'\\' => out.push(b'\\'),
                                        b'/' => out.push(b'/'),
                                        b'b' => out.push(0x08),
                                        b'f' => out.push(0x0C),
                                        b'n' => out.push(b'\n'),
                                        b'r' => out.push(b'\r'),
                                        b't' => out.push(b'\t'),
                                        b'u' => {
                                                let mut cp = self.parse_hex4();
                                                if (0xD800..=0xDBFF).contains(&cp) {
                                                        if self.pos + 6 <= self.src.len() && self.src[self.pos] == b'\\' && self.src[self.pos + 1] == b'u' {
                                                                self.pos += 2;
                                                                let lo = self.parse_hex4();
                                                                cp = if (0xDC00..=0xDFFF).contains(&lo) { 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) } else { 0xFFFD };
                                                        } else {
                                                                cp = 0xFFFD;
                                                        }
                                                } else if (0xDC00..=0xDFFF).contains(&cp) {
                                                        cp = 0xFFFD;
                                                }
                                                let ch = char::from_u32(cp).unwrap_or('\u{FFFD}');
                                                let mut buf = [0u8; 4];
                                                out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
                                        }
                                        other => out.push(other),
                                }
                        }
                        self.error = true;
                        String::from_utf8_lossy(&out).into_owned()
                }

                fn literal(&mut self, word: &[u8], value: Value) -> Value {
                        if self.src.len() - self.pos < word.len() || &self.src[self.pos..self.pos + word.len()] != word {
                                self.error = true;
                                return Value::Null;
                        }
                        self.pos += word.len();
                        value
                }

                fn digits(&mut self) -> usize {
                        let start = self.pos;
                        while self.pos < self.src.len() && self.src[self.pos].is_ascii_digit() { self.pos += 1; }
                        self.pos - start
                }

                fn parse_number(&mut self) -> Value {
                        let start = self.pos;
                        if self.peek() == b'-' { self.pos += 1; }
                        let digit_start = self.pos;
                        let int_digits = self.digits();
                        if int_digits == 0 || (self.src[digit_start] == b'0' && int_digits > 1) { self.error = true; return Value::Null; }
                        let mut integral = true;
                        if self.peek() == b'.' {
                                integral = false;
                                self.pos += 1;
                                if self.digits() == 0 { self.error = true; return Value::Null; }
                        }
                        if matches!(self.peek(), b'e' | b'E') {
                                integral = false;
                                self.pos += 1;
                                if matches!(self.peek(), b'+' | b'-') { self.pos += 1; }
                                if self.digits() == 0 { self.error = true; return Value::Null; }
                        }
                        let text = std::str::from_utf8(&self.src[start..self.pos]).unwrap_or("0");
                        if integral {
                                if let Ok(n) = text.parse::<i64>() { return Value::Int(n); }
                        }
                        Value::Float(text.parse::<f64>().unwrap_or(0.0))
                }

                fn parse_value(&mut self, depth: i32) -> Value {
                        if self.error { return Value::Null; }
                        if depth > MAX_DEPTH { self.error = true; return Value::Null; }
                        self.skip_ws();
                        match self.peek() {
                                b'"' => Value::String(self.parse_string()),
                                b'{' => {
                                        self.consume();
                                        let mut entries = Vec::new();
                                        self.skip_ws();
                                        if self.peek() == b'}' { self.consume(); return Value::Object(entries); }
                                        while !self.error {
                                                self.skip_ws();
                                                let key = self.parse_string();
                                                if self.error { break; }
                                                if !self.expect(b':') { break; }
                                                let val = self.parse_value(depth + 1);
                                                entries.push((key, val));
                                                if self.error { break; }
                                                self.skip_ws();
                                                match self.peek() {
                                                        b',' => { self.consume(); }
                                                        b'}' => { self.consume(); break; }
                                                        _ => { self.error = true; break; }
                                                }
                                        }
                                        Value::Object(entries)
                                }
                                b'[' => {
                                        self.consume();
                                        let mut items = Vec::new();
                                        self.skip_ws();
                                        if self.peek() == b']' { self.consume(); return Value::Array(items); }
                                        while !self.error {
                                                items.push(self.parse_value(depth + 1));
                                                if self.error { break; }
                                                self.skip_ws();
                                                match self.peek() {
                                                        b',' => { self.consume(); }
                                                        b']' => { self.consume(); break; }
                                                        _ => { self.error = true; break; }
                                                }
                                        }
                                        Value::Array(items)
                                }
                                b't' => self.literal(b"true", Value::Bool(true)),
                                b'f' => self.literal(b"false", Value::Bool(false)),
                                b'n' => self.literal(b"null", Value::Null),
                                _ => self.parse_number(),
                        }
                }
        }

        pub fn parse(json: &str) -> Value {
                let mut p = Parser { src: json.as_bytes(), pos: 0, error: false };
                let v = p.parse_value(0);
                if !p.error { p.skip_ws(); }
                if p.error || p.pos != p.src.len() { return Value::Null; }
                v
        }

        fn trim_fraction(s: &str) -> &str {
                if s.contains('.') { s.trim_end_matches('0').trim_end_matches('.') } else { s }
        }

        fn format_number(f: f64) -> String {
                if !f.is_finite() { return "null".to_string(); }
                if f == 0.0 { return if f.is_sign_negative() { "-0".to_string() } else { "0".to_string() }; }
                let sci = format!("{:.16e}", f);
                let (mantissa, exp) = sci.split_once('e').unwrap_or((&sci, "0"));
                let exp: i32 = exp.parse().unwrap_or(0);
                if exp < -4 || exp >= 17 {
                        format!("{}e{}{:02}", trim_fraction(mantissa), if exp < 0 { '-' } else { '+' }, exp.abs())
                } else {
                        trim_fraction(&format!("{:.*}", (16 - exp) as usize, f)).to_string()
                }
        }

        pub fn stringify_to(out: &mut String, v: &Value) {
                match v {
                        Value::Null | Value::Binary(_) => out.push_str("null"),
                        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
                        Value::Int(n) => out.push_str(&n.to_string()),
                        Value::Float(f) => out.push_str(&format_number(*f)),
                        Value::String(s) => out.push_str(&quote(s)),
                        Value::FuncRef(r) => out.push_str(&quote(r)),
                        Value::Vector2(v) => stringify_components(out, &[v.x, v.y]),
                        Value::Vector3(v) => stringify_components(out, &[v.x, v.y, v.z]),
                        Value::Vector4(v) => stringify_components(out, &[v.x, v.y, v.z, v.w]),
                        Value::Quaternion(q) => stringify_components(out, &[q.x, q.y, q.z, q.w]),
                        Value::Array(items) => {
                                out.push('[');
                                for (i, item) in items.iter().enumerate() {
                                        if i > 0 { out.push(','); }
                                        stringify_to(out, item);
                                }
                                out.push(']');
                        }
                        Value::Object(entries) => {
                                out.push('{');
                                for (i, (k, item)) in entries.iter().enumerate() {
                                        if i > 0 { out.push(','); }
                                        out.push_str(&quote(k));
                                        out.push(':');
                                        stringify_to(out, item);
                                }
                                out.push('}');
                        }
                }
        }

        fn stringify_components(out: &mut String, components: &[f32]) {
                out.push('{');
                for (i, (name, c)) in ["x", "y", "z", "w"].iter().zip(components).enumerate() {
                        if i > 0 { out.push(','); }
                        out.push_str(&quote(name));
                        out.push(':');
                        if c.is_finite() { out.push_str(&c.to_string()); } else { out.push_str("null"); }
                }
                out.push('}');
        }

        pub fn stringify(v: &Value) -> String {
                let mut out = String::new();
                stringify_to(&mut out, v);
                out
        }
}

pub fn native_void(name: &str, args: &[NativeArg]) {
        invoke_native_void(hash_string(name.as_bytes()), args);
}

pub fn get_players() -> Vec<i32> {
        (0..native_int("GET_NUM_PLAYER_INDICES", &[]))
                .filter_map(|i| native_string("GET_PLAYER_FROM_INDEX", &[NativeArg::val(i as u32 as u64)]).parse().ok())
                .collect()
}

pub fn get_player_identifiers(source: i32) -> Vec<String> {
        let player = CString::new(source.to_string()).unwrap_or_default();
        let count = native_int("GET_NUM_PLAYER_IDENTIFIERS", &[NativeArg::ptr(player.as_ptr() as u64)]);
        (0..count).map(|i| native_string("GET_PLAYER_IDENTIFIER", &[NativeArg::ptr(player.as_ptr() as u64), NativeArg::val(i as u32 as u64)])).collect()
}

pub fn native_int(name: &str, args: &[NativeArg]) -> i32 {
        invoke_native_int(hash_string(name.as_bytes()), args)
}

pub fn native_i64(name: &str, args: &[NativeArg]) -> i64 {
        invoke_native_i64(hash_string(name.as_bytes()), args)
}

pub fn native_float(name: &str, args: &[NativeArg]) -> f32 {
        invoke_native_float(hash_string(name.as_bytes()), args)
}

pub fn native_bool(name: &str, args: &[NativeArg]) -> bool {
        invoke_native_bool(hash_string(name.as_bytes()), args)
}

pub fn native_string(name: &str, args: &[NativeArg]) -> String {
        invoke_native_string(hash_string(name.as_bytes()), args)
}

pub fn native_vector3(name: &str, args: &[NativeArg]) -> Vector3 {
        invoke_native_vector3(hash_string(name.as_bytes()), args)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ArgError {
        pub index: usize,
        pub reason: String,
}

pub trait FromArg: Sized {
        fn from_arg(v: &Value) -> Result<Self, String>;
        fn from_missing() -> Result<Self, String> { Err("missing".to_string()) }
}

pub trait FromArgs: Sized {
        fn from_args(args: &EventArgs) -> Result<Self, ArgError>;
}

pub fn decode_args<T: FromArgs>(args: &EventArgs) -> Result<T, ArgError> {
        T::from_args(args)
}

fn arg_integer(v: &Value) -> Result<i64, String> {
        match *v {
                Value::Int(n) => Ok(n),
                Value::Float(f) if f.is_finite() && f.trunc() == f => {
                        if f < -9223372036854775808.0 || f >= 9223372036854775808.0 { return Err("integer out of range".to_string()); }
                        Ok(f as i64)
                }
                _ => Err("expected integer".to_string()),
        }
}

fn arg_number(v: &Value) -> Result<f64, String> {
        let f = match *v {
                Value::Int(n) => n as f64,
                Value::Float(f) => f,
                _ => return Err("expected number".to_string()),
        };
        if !f.is_finite() { return Err("number must be finite".to_string()); }
        Ok(f)
}

impl FromArg for bool {
        fn from_arg(v: &Value) -> Result<Self, String> {
                match *v { Value::Bool(b) => Ok(b), _ => Err("expected bool".to_string()) }
        }
}

macro_rules! impl_int_from_arg {
        ($($t:ty),*) => {$(
                impl FromArg for $t {
                        fn from_arg(v: &Value) -> Result<Self, String> {
                                <$t>::try_from(arg_integer(v)?).map_err(|_| "integer out of range".to_string())
                        }
                }
        )*};
}
impl_int_from_arg!(i8, i16, i32, i64, u8, u16, u32, u64, isize, usize);

impl FromArg for f64 {
        fn from_arg(v: &Value) -> Result<Self, String> { arg_number(v) }
}

impl FromArg for f32 {
        fn from_arg(v: &Value) -> Result<Self, String> {
                let f = arg_number(v)?;
                if f.abs() > f32::MAX as f64 { return Err("number out of range".to_string()); }
                Ok(f as f32)
        }
}

impl FromArg for String {
        fn from_arg(v: &Value) -> Result<Self, String> {
                match v { Value::String(s) => Ok(s.clone()), _ => Err("expected string".to_string()) }
        }
}

impl FromArg for Value {
        fn from_arg(v: &Value) -> Result<Self, String> { Ok(v.clone()) }
}

impl FromArg for Vector2 {
        fn from_arg(v: &Value) -> Result<Self, String> {
                v.components(2, false).map(|c| Vector2 { x: c[0], y: c[1] }).ok_or_else(|| "expected vector2".to_string())
        }
}

impl FromArg for Vector3 {
        fn from_arg(v: &Value) -> Result<Self, String> {
                v.components(3, false).map(|c| Vector3 { x: c[0], y: c[1], z: c[2] }).ok_or_else(|| "expected vector3".to_string())
        }
}

impl FromArg for Vector4 {
        fn from_arg(v: &Value) -> Result<Self, String> {
                v.components(4, false).map(|c| Vector4 { x: c[0], y: c[1], z: c[2], w: c[3] }).ok_or_else(|| "expected vector4".to_string())
        }
}

impl FromArg for Quaternion {
        fn from_arg(v: &Value) -> Result<Self, String> {
                v.components(4, true).map(|c| Quaternion { x: c[0], y: c[1], z: c[2], w: c[3] }).ok_or_else(|| "expected quaternion".to_string())
        }
}

impl<T: FromArg> FromArg for Option<T> {
        fn from_arg(v: &Value) -> Result<Self, String> {
                match v { Value::Null => Ok(None), other => T::from_arg(other).map(Some) }
        }
        fn from_missing() -> Result<Self, String> { Ok(None) }
}

impl<T: FromArg> FromArg for Vec<T> {
        fn from_arg(v: &Value) -> Result<Self, String> {
                let items = match v { Value::Array(items) => items, _ => return Err("expected array".to_string()) };
                items.iter().enumerate().map(|(i, item)| T::from_arg(item).map_err(|e| format!("element {}: {}", i, e))).collect()
        }
}

fn decode_arg<T: FromArg>(args: &EventArgs, index: usize) -> Result<T, ArgError> {
        let result = if index >= args.len() { T::from_missing() } else { T::from_arg(args.value(index)) };
        result.map_err(|reason| ArgError { index, reason })
}

fn check_arg_count(args: &EventArgs, count: usize) -> Result<(), ArgError> {
        for index in count..args.len() {
                if !args.is_null(index) { return Err(ArgError { index, reason: "unexpected argument".to_string() }); }
        }
        Ok(())
}

impl FromArgs for () {
        fn from_args(args: &EventArgs) -> Result<Self, ArgError> { check_arg_count(args, 0) }
}

macro_rules! impl_from_args {
        ($count:expr; $($t:ident $i:tt),+) => {
                impl<$($t: FromArg),+> FromArgs for ($($t,)+) {
                        fn from_args(args: &EventArgs) -> Result<Self, ArgError> {
                                check_arg_count(args, $count)?;
                                Ok(($(decode_arg::<$t>(args, $i)?,)+))
                        }
                }
        };
}
impl_from_args!(1; A 0);
impl_from_args!(2; A 0, B 1);
impl_from_args!(3; A 0, B 1, C 2);
impl_from_args!(4; A 0, B 1, C 2, D 3);
impl_from_args!(5; A 0, B 1, C 2, D 3, E 4);
impl_from_args!(6; A 0, B 1, C 2, D 3, E 4, F 5);
impl_from_args!(7; A 0, B 1, C 2, D 3, E 4, F 5, G 6);
impl_from_args!(8; A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7);
impl_from_args!(9; A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8);
impl_from_args!(10; A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8, J 9);
impl_from_args!(11; A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8, J 9, K 10);
impl_from_args!(12; A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8, J 9, K 10, L 11);
pub mod runtime {
        use super::*;

        pub fn init(main_fn: fn()) {
                unsafe { *CTX.get() = Some(Context::new()); }
                on("onServerResourceStop", |_source: i32, args: &EventArgs| {
                        if args.len() == 0 { return; }
                        let resource = args.str(0);
                        let prefix = format!("{}/", resource);
                        let c = ctx();
                        unsafe { (*c).export_ref_cache.retain(|k, _| !k.starts_with(&prefix)) };
                });
                main_fn();
        }

        pub fn on_tick() {
                release_pending_refs();
                if has_due_coroutines() {
                        let c = ctx();
                        let now = unsafe { (*c).now_ms() };
                        let mut next = u64::MAX;
                        let mut ready = Vec::new();
                        for (id, e) in unsafe { (*c).coroutines.iter() } {
                                if now >= e.resume_at {
                                        ready.push(*id);
                                } else {
                                        next = next.min(e.resume_at);
                                }
                        }
                        unsafe { (*c).earliest_resume = next };
                        for id in ready {
                                resume_coroutine_by_id(id);
                        }
                }
                let c = ctx();
                let now = unsafe { (*c).now_ms() };
                let expired: Vec<i32> = unsafe { (*c).timers.iter() }
                        .filter(|(_, t)| now >= t.next_fire)
                        .map(|(id, _)| *id)
                        .collect();
                for id in expired {
                        let c = ctx();
                        if let Some(timer) = unsafe { (*c).timers.get_mut(&id) } {
                                if timer.repeat {
                                        let next = timer.next_fire + timer.interval_ms as u64;
                                        timer.next_fire = if next > now { next } else { now };
                                        let cb = Rc::clone(&timer.callback);
                                        cb();
                                } else {
                                        let timer = unsafe { (*c).timers.remove(&id) }.unwrap();
                                        (timer.callback)();
                                }
                        }
                }
                let tick_snapshot: Vec<Rc<dyn Fn()>> = unsafe { (*ctx()).ticks.values() }.map(Rc::clone).collect();
                for h in tick_snapshot {
                        h();
                }
        }

        fn parse_source(src: &str) -> i32 {
                src.strip_prefix("net:").or_else(|| src.strip_prefix("internal-net:")).and_then(|id| id.parse().ok()).unwrap_or(0)
        }

        pub fn on_event(name: *const u8, name_len: u32, args: *const u8, args_len: u32, src: *const u8, src_len: u32) {
                let name_bytes = unsafe { std::slice::from_raw_parts(name, name_len as usize) };
                let key = match std::str::from_utf8(name_bytes) { Ok(s) => s, Err(_) => return };
                let src_bytes = unsafe { std::slice::from_raw_parts(src, src_len as usize) };
                let src_str = match std::str::from_utf8(src_bytes) { Ok(s) => s, Err(_) => return };
                let args_data = unsafe { std::slice::from_raw_parts(args, args_len as usize) };

                let c = ctx();
                if unsafe { !(*c).events.contains_key(key) } { return; }

                if src_str.starts_with("net:") {
                        if unsafe { !(*c).net_safe_events.contains(key) } { return; }
                }

                let decoded = ensure_array(decode(args_data));
                let ea = EventArgs::new(decoded);

                let handlers: Vec<EventHandlerFn> = unsafe { (*c).events.get(key) }
                        .map(|v| v.iter().map(|e| Rc::clone(&e.handler)).collect())
                        .unwrap_or_default();

                let source = parse_source(src_str);
                for h in handlers {
                        h(source, &ea);
                }
        }

        pub fn on_stop() {
                let c = ctx();
                unsafe { (*c).coroutines.clear() };
                unsafe { (*c).export_ref_cache.clear() };
                let refs: Vec<(i32, i32)> = unsafe { (*c).state_bag_handler_refs.drain() }.collect();
                for (cookie, host_ref) in refs {
                        invoke_native_void(hash_string(b"REMOVE_STATE_BAG_CHANGE_HANDLER"), &[NativeArg::val(cookie as u32 as u64)]);
                        remove_ref_idx(host_ref);
                }
                let snapshot: Vec<Rc<dyn Fn()>> = unsafe { (*c).stops.values() }.map(Rc::clone).collect();
                for h in snapshot {
                        h();
                }
                release_pending_refs();
        }

        pub fn alloc(size: u32) -> *mut u8 {
                if size == 0 { return std::ptr::null_mut(); }
                unsafe {
                        std::alloc::alloc(std::alloc::Layout::from_size_align_unchecked(size as usize, 1))
                }
        }

        pub fn free(ptr: *mut u8, size: u32) {
                if ptr.is_null() || size == 0 { return; }
                unsafe {
                        std::alloc::dealloc(ptr, std::alloc::Layout::from_size_align_unchecked(size as usize, 1));
                }
        }

        pub fn has_pending_work() -> i32 {
                let c = ctx();
                if unsafe { !(*c).ticks.is_empty() || !(*c).timers.is_empty() || !(*PENDING_REF_RELEASES.get()).is_empty() } || has_due_coroutines() { 1 } else { 0 }
        }

        pub fn invoke_ref(callback_id: i32, args_ptr: *const u8, args_len: u32, result_ptr: *mut u8, result_max: u32) -> i32 {
                let c = ctx();
                let result = match unsafe { (*c).pending_ref_result.take() } {
                        Some((id, data)) if id == callback_id && data.len() as u32 <= result_max => data,
                        _ => {
                                let cb = match unsafe { (*c).ref_callbacks.get(&callback_id) } { Some(cb) => Rc::clone(cb), None => return 0 };
                                let args = unsafe { std::slice::from_raw_parts(args_ptr, args_len as usize) };
                                cb(args)
                        }
                };
                let copy = (result.len() as u32).min(result_max);
                if copy > 0 && !result_ptr.is_null() {
                        unsafe { std::ptr::copy_nonoverlapping(result.as_ptr(), result_ptr, copy as usize); }
                }
                let size = result.len() as i32;
                if result.len() as u32 > result_max {
                        unsafe { (*c).pending_ref_result = Some((callback_id, result)) };
                }
                size
        }

        pub fn duplicate_ref(callback_id: i32) -> i32 {
                let c = ctx();
                if let Some(count) = unsafe { (*c).ref_counts.get_mut(&callback_id) } {
                        *count += 1;
                }
                callback_id
        }

        pub fn remove_ref_callback(callback_id: i32) {
                let c = ctx();
                let remove = if let Some(count) = unsafe { (*c).ref_counts.get_mut(&callback_id) } {
                        *count -= 1;
                        *count <= 0
                } else { false };
                if remove {
                        unsafe { (*c).ref_counts.remove(&callback_id) };
                        unsafe { (*c).ref_callbacks.remove(&callback_id) };
                }
        }

        pub fn tick_bookmarks(bookmarks: *const i32, count: i32) {
                if count <= 0 { return; }
                let ids = unsafe { std::slice::from_raw_parts(bookmarks, count as usize) };
                for &id in ids {
                        resume_coroutine_if_due(id);
                }
        }
}
pub mod natives {
        #[cfg(cfx_has_natives)]
        include!("../src/natives/all.rs");
}
#[macro_export]
macro_rules! trace {
        ($($arg:tt)*) => {{
                let msg = format!($($arg)*);
                $crate::trace_str(&msg);
        }};
}

#[macro_export]
macro_rules! trace_ln {
        ($($arg:tt)*) => {{
                let mut msg = format!($($arg)*);
                if !msg.ends_with('\n') { msg.push('\n'); }
                $crate::trace_str(&msg);
        }};
}

#[macro_export]
macro_rules! Server {
        { $($body:tt)* } => {
                fn __cfx_user_main() {
                        $($body)*
                }
                $crate::__cfx_define_exports!(__cfx_user_main);
        };
}

#[macro_export]
macro_rules! cfx_worker {
        (fn $name:ident($input:ident: &[u8], $result:ident: &mut [u8]) -> i32 $body:block) => {
                #[no_mangle]
                pub extern "C" fn $name(ip: i32, il: i32, rp: i32, rm: i32) -> i32 {
                        let $input: &[u8] = if il > 0 && ip != 0 {
                                unsafe { core::slice::from_raw_parts(ip as *const u8, il as usize) }
                        } else {
                                &[]
                        };
                        let $result: &mut [u8] = if rm > 0 && rp != 0 {
                                unsafe { core::slice::from_raw_parts_mut(rp as *mut u8, rm as usize) }
                        } else {
                                &mut []
                        };
                        $body
                }
        };
}

#[macro_export]
#[doc(hidden)]
macro_rules! __cfx_define_exports {
        ($main_fn:ident) => {
                #[no_mangle]
                pub extern "C" fn __cfx_init() {
                        $crate::runtime::init($main_fn);
                }

                #[no_mangle]
                pub extern "C" fn __cfx_on_tick() {
                        $crate::runtime::on_tick();
                }

                #[no_mangle]
                pub extern "C" fn __cfx_on_event(name: *const u8, name_len: u32, args: *const u8, args_len: u32, src: *const u8, src_len: u32) {
                        $crate::runtime::on_event(name, name_len, args, args_len, src, src_len);
                }

                #[no_mangle]
                pub extern "C" fn __cfx_on_stop() {
                        $crate::runtime::on_stop();
                }

                #[no_mangle]
                pub extern "C" fn __cfx_alloc(size: u32) -> *mut u8 {
                        $crate::runtime::alloc(size)
                }

                #[no_mangle]
                pub extern "C" fn __cfx_free(ptr: *mut u8, size: u32) {
                        $crate::runtime::free(ptr, size);
                }

                #[no_mangle]
                pub extern "C" fn __cfx_has_pending_work() -> i32 {
                        $crate::runtime::has_pending_work()
                }

                #[no_mangle]
                pub extern "C" fn __cfx_invoke_ref(callback_id: i32, args_ptr: *const u8, args_len: u32, result_ptr: *mut u8, result_max: u32) -> i32 {
                        $crate::runtime::invoke_ref(callback_id, args_ptr, args_len, result_ptr, result_max)
                }

                #[no_mangle]
                pub extern "C" fn __cfx_duplicate_ref(callback_id: i32) -> i32 {
                        $crate::runtime::duplicate_ref(callback_id)
                }

                #[no_mangle]
                pub extern "C" fn __cfx_remove_ref(callback_id: i32) {
                        $crate::runtime::remove_ref_callback(callback_id);
                }

                #[no_mangle]
                pub extern "C" fn __cfx_tick_bookmarks(bookmarks: *const i32, count: i32) {
                        $crate::runtime::tick_bookmarks(bookmarks, count);
                }
        };
}
