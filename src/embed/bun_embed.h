/// bun_embed.h — High-performance C API for embedding Bun's JS runtime.
///
/// Design goals:
///   - Zero-copy value passing between C and JavaScript (BunValue = uint64_t)
///   - No JSON bridge overhead
///   - Direct registration of functions/objects/getters/setters
///
/// Typical flow:
///   1. bun_initialize()
///   2. bun_context() to get JS context
///   3. Register values/functions on global object
///   4. bun_eval_string()/bun_eval_file() with that context
///   5. bun_run_pending_jobs() in host loop
///   6. bun_destroy()

#ifndef BUN_EMBED_H
#define BUN_EMBED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to a Bun runtime instance.
typedef struct BunRuntime BunRuntime;

/// Opaque execution context (backed by JSGlobalObject* internally).
typedef struct BunContext BunContext;

/// Opaque runtime-local class handle returned by bun_class_register().
typedef struct BunClass BunClass;

/// Encoded JavaScript value (NaN-boxed JSValue).
typedef uint64_t BunValue;

/// Host function callback callable from JavaScript.
/// argv points to contiguous BunValue arguments valid only for this call.
/// To raise a JavaScript exception from a callback, call bun_throw(ctx, err)
/// and return promptly.
typedef BunValue (*BunHostFn)(BunContext* ctx, int argc, const BunValue* argv, void* userdata);

/// Custom getter callback for bun_define_accessor().
/// To raise a JavaScript exception from a callback, call bun_throw(ctx, err)
/// and return promptly.
typedef BunValue (*BunGetterFn)(BunContext* ctx, BunValue this_value, void* userdata);

/// Custom setter callback for bun_define_accessor().
/// To raise a JavaScript exception from a callback, call bun_throw(ctx, err)
/// and return promptly.
typedef void (*BunSetterFn)(BunContext* ctx, BunValue this_value, BunValue value, void* userdata);

/// Finalizer callback for bun_define_finalizer().
///
/// This runs during GC finalization. Do not call Bun or JS APIs from it,
/// except bun_unprotect() on values that were previously bun_protect()'d and
/// are being released here. All other Bun or JS APIs are unsupported.
/// Use it only to release native resources associated with the object.
/// Each object may have at most one user finalizer registered via
/// bun_define_finalizer().
typedef void (*BunFinalizerFn)(void* userdata);

/// Native instance method callback for bun_class_register().
/// To raise a JavaScript exception from a callback, call bun_throw(ctx, err)
/// and return promptly.
typedef BunValue (*BunClassMethodFn)(BunContext* ctx, BunValue this_value, void* native_ptr,
    int argc, const BunValue* argv, void* userdata);

/// Native property getter callback for bun_class_register().
/// To raise a JavaScript exception from a callback, call bun_throw(ctx, err)
/// and return promptly.
typedef BunValue (*BunClassGetterFn)(BunContext* ctx, BunValue this_value, void* native_ptr, void* userdata);

/// Native property setter callback for bun_class_register().
/// To raise a JavaScript exception from a callback, call bun_throw(ctx, err)
/// and return promptly.
typedef void (*BunClassSetterFn)(BunContext* ctx, BunValue this_value, void* native_ptr,
    BunValue value, void* userdata);

/// Native constructor callback for bun_class_register().
///
/// This is invoked for JavaScript `new` calls on the class constructor returned
/// by bun_class_constructor(). The callback should typically allocate native
/// state and return a BunClass instance created via bun_class_new(). To raise
/// a JavaScript exception instead, call bun_throw(ctx, err) and return
/// promptly.
typedef BunValue (*BunClassConstructorFn)(BunContext* ctx, BunClass* klass,
    int argc, const BunValue* argv, void* userdata);

/// Native static method callback for bun_class_register().
/// To raise a JavaScript exception from a callback, call bun_throw(ctx, err)
/// and return promptly.
typedef BunValue (*BunClassStaticMethodFn)(BunContext* ctx, BunValue this_value,
    void* userdata, int argc, const BunValue* argv);

/// Native static property getter callback for bun_class_register().
/// To raise a JavaScript exception from a callback, call bun_throw(ctx, err)
/// and return promptly.
typedef BunValue (*BunClassStaticGetterFn)(BunContext* ctx, BunValue this_value,
    void* userdata);

/// Native static property setter callback for bun_class_register().
/// To raise a JavaScript exception from a callback, call bun_throw(ctx, err)
/// and return promptly.
typedef void (*BunClassStaticSetterFn)(BunContext* ctx, BunValue this_value,
    BunValue value, void* userdata);

/// Finalizer callback for bun_class_new().
///
/// Runs at most once, either when bun_class_dispose() is called or when the JS
/// object is eventually GC'd. Do not call Bun or JS APIs from it, except
/// bun_unprotect() on values that were previously bun_protect()'d and are
/// being released here. All other Bun or JS APIs are unsupported.
typedef void (*BunClassFinalizerFn)(void* native_ptr, void* userdata);

typedef struct {
    const char* name;
    size_t name_len;
    BunClassMethodFn callback;
    void* userdata;
    int arg_count;
    int dont_enum;
    int dont_delete;
} BunClassMethodDescriptor;

typedef struct {
    const char* name;
    size_t name_len;
    BunClassGetterFn getter;
    BunClassSetterFn setter;
    void* userdata;
    int read_only;
    int dont_enum;
    int dont_delete;
} BunClassPropertyDescriptor;

typedef struct {
    const char* name;
    size_t name_len;
    BunClassStaticMethodFn callback;
    void* userdata;
    int arg_count;
    int dont_enum;
    int dont_delete;
} BunClassStaticMethodDescriptor;

typedef struct {
    const char* name;
    size_t name_len;
    BunClassStaticGetterFn getter;
    BunClassStaticSetterFn setter;
    void* userdata;
    int read_only;
    int dont_enum;
    int dont_delete;
} BunClassStaticPropertyDescriptor;

typedef struct {
    const char* name;
    size_t name_len;
    const BunClassPropertyDescriptor* properties;
    size_t property_count;
    const BunClassMethodDescriptor* methods;
    size_t method_count;
    BunClassConstructorFn constructor;
    void* constructor_userdata;
    int constructor_arg_count;
    const BunClassStaticPropertyDescriptor* static_properties;
    size_t static_property_count;
    const BunClassStaticMethodDescriptor* static_methods;
    size_t static_method_count;
} BunClassDescriptor;

/// Predefined immediate values in JSValue64 mode.
#define BUN_UNDEFINED ((BunValue)0xAULL)
#define BUN_NULL ((BunValue)0x2ULL)
#define BUN_TRUE ((BunValue)0x7ULL)
#define BUN_FALSE ((BunValue)0x6ULL)

/// Sentinel returned by bun_call() when a JavaScript exception was thrown.
/// This is JSC's internal zero/exception sentinel and is never a valid
/// return value from a successful call.
#define BUN_EXCEPTION ((BunValue)0ULL)

/// Debugger startup mode for bun_initialize().
typedef enum {
    BUN_DEBUGGER_OFF = 0,
    /// Start the inspector and begin executing immediately.
    BUN_DEBUGGER_ATTACH = 1,
    /// Start the inspector and wait for a debugger client before executing.
    BUN_DEBUGGER_WAIT = 2,
    /// Wait for a debugger client and pause on the first line.
    BUN_DEBUGGER_BREAK = 3,
} BunDebuggerMode;

/// Initialization options for bun_initialize().
typedef struct {
    /// Working directory (UTF-8, null-terminated). Pass NULL for current dir.
    const char* cwd;
    /// Debugger startup mode. Zero / BUN_DEBUGGER_OFF disables debugging.
    BunDebuggerMode debugger_mode;
    /// Optional inspector listen URL or path. Pass NULL to use Bun's default.
    /// Examples:
    ///   "6499"                        — listen on ws://localhost:6499/<random>
    ///   "127.0.0.1:6499"              — listen on a specific interface
    ///   "ws://0.0.0.0:6499/debug"     — listen with a fixed WebSocket path
    ///   "unix:///tmp/bun-debug.sock"  — listen on a Unix socket
    const char* debugger_listen_url;
} BunInitializeOptions;

/// Result of a non-blocking embed event-loop tick.
typedef enum {
    /// Runtime is fully idle: no queued work and no active handles/timers.
    BUN_PENDING_JOBS_IDLE = 0,
    /// More work can be processed immediately without waiting for a new wakeup.
    BUN_PENDING_JOBS_SPIN = 1,
    /// Runtime is still active, but further progress requires a future wakeup.
    BUN_PENDING_JOBS_WAIT = 2,
} BunPendingJobsResult;

// --------------------------------------------------------------------------
// Lifecycle
// --------------------------------------------------------------------------

/// Initialize a Bun runtime instance. Must be called from the thread that will
/// drive the event loop (typically the main/GUI thread).
/// @param options  Initialization options, or NULL for defaults.
/// @return  Runtime handle, or NULL on failure.
BunRuntime* bun_initialize(const BunInitializeOptions* options);

/// Destroy a Bun runtime and free all resources.
void bun_destroy(BunRuntime* rt);

/// Get the unique JS context for this runtime.
///
/// Bun currently exposes exactly one context per runtime. This function does
/// not create a new context; repeated calls return the same handle.
BunContext* bun_context(BunRuntime* rt);

// --------------------------------------------------------------------------
// Evaluation
// --------------------------------------------------------------------------

/// Evaluate JavaScript/TypeScript source with script semantics.
///
/// @param ctx       JS context.
/// @param code      UTF-8 source code bytes.
/// @param code_len  Length of code in bytes.
/// @return      The JS completion value on success, or BUN_EXCEPTION (0) if an
///              error occurred. Call bun_last_error(ctx, NULL) to retrieve the
///              error message after a BUN_EXCEPTION return. The error string is valid
///              until the next bun_eval*()/bun_call() call on this context.
BunValue bun_eval_string(BunContext* ctx, const char* code, size_t code_len);

/// Load and evaluate a JavaScript/TypeScript file as an ES module.
///
/// ES modules do not produce a meaningful completion value, so BUN_UNDEFINED is
/// returned on success. On failure BUN_EXCEPTION (0) is returned and the error
/// message is available via bun_last_error(ctx, NULL).
///
/// @param ctx       JS context.
/// @param path      UTF-8 file path bytes. Relative paths resolve from cwd.
/// @param path_len  Length of path in bytes.
/// @return      BUN_UNDEFINED on success, BUN_EXCEPTION (0) on failure.
BunValue bun_eval_file(BunContext* ctx, const char* path, size_t path_len);

// --------------------------------------------------------------------------
// Event Loop Integration
// --------------------------------------------------------------------------
//
// Three integration modes — pick the one that matches your host application:
//
// ┌──────────────────────────────────────────────────────────────────────┐
// │ Mode A — Callback-driven  (recommended for GUI apps: SDL, GTK, Qt) │
// ├──────────────────────────────────────────────────────────────────────┤
// │ Use bun_set_event_callback() to register a callback.  An internal  │
// │ background thread monitors I/O + timers and invokes your callback   │
// │ (from the background thread) whenever work is ready.  Your callback │
// │ should wake the main thread via a thread-safe mechanism.            │
// │                                                                     │
// │   // Setup (once):                                                  │
// │   void on_bun_ready(void* ud) {                                    │
// │       SDL_Event ev = { .type = SDL_USEREVENT };                    │
// │       SDL_PushEvent(&ev);  // thread-safe                          │
// │   }                                                                │
// │   bun_set_event_callback(rt, on_bun_ready, NULL);                  │
// │                                                                     │
// │   // Main loop:                                                     │
// │   while (running) {                                                │
// │       SDL_WaitEvent(&ev);  // blocks; Bun callback wakes us        │
// │       handle_sdl_event(ev);                                        │
// │       BunPendingJobsResult r;                                      │
// │       while ((r = bun_run_pending_jobs(rt)) == BUN_PENDING_JOBS_SPIN) {}│
// │   }                                                                │
// │                                                                     │
// │ Zero CPU while idle. Sub-ms latency for I/O, wakeups, and timers.  │
// │ Works on all platforms (POSIX + Windows).                           │
// └──────────────────────────────────────────────────────────────────────┘
//
// ┌──────────────────────────────────────────────────────────────────────┐
// │ Mode B — fd merge  (custom event loops with their own poll/epoll)   │
// ├──────────────────────────────────────────────────────────────────────┤
// │ Add bun_get_event_fd() to your existing poll set and use            │
// │ bun_get_wait_hint() as the timeout so JS timers fire on time.      │
// │ POSIX only — returns -1 on Windows.                                │
// │                                                                     │
// │   int bun_fd = bun_get_event_fd(rt);                               │
// │   struct pollfd fds[2] = {                                         │
// │       { .fd = my_app_fd, .events = POLLIN },                      │
// │       { .fd = bun_fd,    .events = POLLIN },                      │
// │   };                                                                │
// │   while (running) {                                                │
// │       int64_t hint = bun_get_wait_hint(rt);                        │
// │       int tmo = (hint < 0) ? my_default_tmo : (int)hint;          │
// │       poll(fds, 2, tmo);                                           │
// │       if (fds[0].revents) handle_app_events();                     │
// │       BunPendingJobsResult r;                                      │
// │       while ((r = bun_run_pending_jobs(rt)) == BUN_PENDING_JOBS_SPIN) {}│
// │   }                                                                │
// └──────────────────────────────────────────────────────────────────────┘
//
// ┌──────────────────────────────────────────────────────────────────────┐
// │ Mode C — Frame-driven polling  (game loops with fixed vsync/tick)   │
// ├──────────────────────────────────────────────────────────────────────┤
// │ Call bun_run_pending_jobs() once per frame.  No fd, no callback.   │
// │ Timer precision equals your frame interval (~16ms at 60 fps).      │
// │ Simplest integration but highest latency.                           │
// │                                                                     │
// │   while (running) {                                                │
// │       // ... input, update, render ...                             │
// │       BunPendingJobsResult r;                                      │
// │       while ((r = bun_run_pending_jobs(rt)) == BUN_PENDING_JOBS_SPIN) {}│
// │       present_frame();                                             │
// │   }                                                                │
// └──────────────────────────────────────────────────────────────────────┘

/// Drive the Bun event loop non-blockingly. Call this from your main thread
/// to process pending timers, promises, I/O callbacks, etc.
///
/// Performs a single non-blocking tick: processes all currently ready events
/// and returns immediately.  It will NOT block waiting for new events.
///
/// Return-value semantics:
///   - BUN_PENDING_JOBS_IDLE: fully idle — no active handles or pending work.
///   - BUN_PENDING_JOBS_SPIN: more work is runnable immediately; call again.
///   - BUN_PENDING_JOBS_WAIT: runtime is active but waiting for I/O or timers;
///     return to your host loop.
///
/// @param rt  Runtime handle.
/// @return    Tri-state result describing whether to stop, spin, or wait.
BunPendingJobsResult bun_run_pending_jobs(BunRuntime* rt);

/// Get the underlying OS event loop file descriptor (epoll fd on Linux,
/// kqueue fd on macOS).  The fd becomes readable when I/O is ready or
/// bun_wakeup()/bun_call_async() is called from another thread.
///
/// JS timers are NOT signaled through this fd — use bun_get_wait_hint() as
/// the timeout when polling so timers fire on time.
///
/// Returns -1 on Windows (IOCP has no pollable fd) or if unavailable.
/// Intended for Mode B (fd merge) integration only.
///
/// @param rt  Runtime handle.
/// @return    Pollable fd, or -1 if unavailable.
int bun_get_event_fd(BunRuntime* rt);

/// Return the recommended wait timeout in milliseconds for the embed host.
///
/// Call this before blocking in your poll/select to determine how long to
/// wait before the next JS timer fires.
///
/// Return values:
///    0  — work is runnable right now; call bun_run_pending_jobs() immediately.
///   -1  — no JS timers pending; wait indefinitely on I/O / bun_wakeup().
///   >0  — milliseconds until the next JS timer fires; use as poll/select
///         timeout on the fd from bun_get_event_fd().
///
/// Intended for Mode B (fd merge) integration.
/// In Mode A (callback), the internal watcher thread uses this automatically.
/// In Mode C (frame-driven), this is not needed.
///
/// @param rt  Runtime handle.
/// @return    Timeout in ms, 0 for immediate, -1 for indefinite.
int64_t bun_get_wait_hint(BunRuntime* rt);

/// Thread-safe: wake up the event loop from any thread. After calling this,
/// the next bun_run_pending_jobs() will process any concurrently queued work.
///
/// NOTE: bun_call_async() already calls this internally after enqueuing the
/// call, so there is no need to follow bun_call_async() with bun_wakeup().
void bun_wakeup(BunRuntime* rt);

/// Callback type invoked from Bun's internal background thread when the
/// runtime has ready work.
///
/// THREAD SAFETY: Called from a background thread, NOT the host's main thread.
/// The callback MUST be thread-safe — use SDL_PushEvent, PostMessage,
/// write(pipe_fd, …), pthread_cond_signal, etc. to wake your main thread.
///
/// The callback fires at most once per batch: after firing, the watcher waits
/// for bun_run_pending_jobs() to acknowledge before notifying again.
///
/// @param userdata  The pointer passed to bun_set_event_callback().
typedef void (*BunEventCallback)(void* userdata);

/// Register a callback to wake the host loop when work is ready (Mode A).
///
/// Starts an internal background thread that monitors all event sources
/// (I/O, timers, cross-thread wakeups) and calls your callback whenever
/// the runtime transitions from idle to having ready work.
///
/// The background thread blocks on OS primitives — zero CPU while idle:
///   POSIX:   poll(kqueue/epoll fd, timer_timeout)
///   Windows: GetQueuedCompletionStatusEx(IOCP, timer_timeout)
/// All event types are covered with sub-millisecond latency.
///
/// Lifecycle:
///   - Each call stops the previous background thread (if any) and starts
///     a new one.  Only the most recently set callback is active.
///   - Pass NULL for cb to stop monitoring without starting a new thread.
///   - Prefer calling once at startup.
///
/// @param rt        Runtime handle.
/// @param cb        Callback invoked when work is ready (NULL to stop).
/// @param userdata  Forwarded verbatim to the callback.
void bun_set_event_callback(BunRuntime* rt, BunEventCallback cb, void* userdata);

// --------------------------------------------------------------------------
// Value Creation
// --------------------------------------------------------------------------

BunValue bun_bool(int value);
BunValue bun_number(double value);
BunValue bun_int32(int32_t value);
BunValue bun_string(BunContext* ctx, const char* utf8, size_t len);
BunValue bun_object(BunContext* ctx);
BunValue bun_array(BunContext* ctx, size_t len);
BunValue bun_global(BunContext* ctx);
BunValue bun_function(BunContext* ctx, const char* name, size_t name_len, BunHostFn fn, void* userdata, int arg_count);

// --------------------------------------------------------------------------
// Error Helpers
// --------------------------------------------------------------------------

/// Construct a JavaScript Error object from a UTF-8 message without throwing it.
///
/// Typical usage:
///   BunValue err = bun_error(ctx, "boom", 4);
///   return bun_throw(ctx, err);
///
/// The returned value may also be stored, passed around, or thrown later.
/// Returns BUN_UNDEFINED on failure.
BunValue bun_error(BunContext* ctx, const char* utf8, size_t len);

/// Raise a pending JavaScript exception from native embed code.
///
/// `err` may be any BunValue from the same runtime/context. JavaScript code
/// catches the original thrown value via try/catch.
///
/// Usage from BunValue-returning callbacks:
///   return bun_throw(ctx, err);
///
/// Usage from setter/void callbacks:
///   bun_throw(ctx, err);
///   return;
///
/// After calling bun_throw(), return promptly so control can unwind back into
/// JavaScript. Passing BUN_EXCEPTION is invalid; bun_throw() raises a
/// TypeError instead.
///
/// @return BUN_EXCEPTION for convenience.
BunValue bun_throw(BunContext* ctx, BunValue err);

/// Element type for bun_typed_array().
typedef enum {
    BUN_INT8_ARRAY = 0,
    BUN_UINT8_ARRAY = 1,
    BUN_UINT8C_ARRAY = 2, ///< Uint8ClampedArray
    BUN_INT16_ARRAY = 3,
    BUN_UINT16_ARRAY = 4,
    BUN_INT32_ARRAY = 5,
    BUN_UINT32_ARRAY = 6,
    BUN_FLOAT32_ARRAY = 7,
    BUN_FLOAT64_ARRAY = 8,
    BUN_BIGINT64_ARRAY = 9,
    BUN_BIGUINT64_ARRAY = 10,
} BunTypedArrayKind;

typedef struct {
    void* data;
    size_t byte_length;
} BunArrayBufferInfo;

typedef struct {
    void* data;
    size_t byte_offset;
    size_t byte_length;
    size_t element_count;
    BunTypedArrayKind kind;
} BunTypedArrayInfo;

/// Wrap C memory as a JS ArrayBuffer (zero-copy).
///
/// The JS ArrayBuffer directly references `data` — no copy is made.
/// When the GC eventually collects the buffer, `finalizer(userdata)` is called.
/// Pass NULL for finalizer only if the memory outlives the entire runtime
/// (e.g. static data, or memory you free yourself at bun_destroy() time).
///
/// MEMORY LIFETIME CONTRACT
///   `data` MUST stay valid until finalizer is called (or until bun_destroy()
///   if no finalizer). Do NOT free `data` before the runtime is destroyed.
///
/// KNOWN LEAK RISK — bun_destroy() does not guarantee finalizer invocation
///   JSC does not guarantee that every in-flight GC finalizer runs during VM
///   teardown. If the runtime is destroyed while a buffer is still reachable
///   from JS (e.g. it was bun_protect()'d and never unprotect()'d, or it is
///   referenced by a long-lived closure), the finalizer may NOT be called.
///   For truly critical resources (sockets, file handles, GPU buffers), keep
///   your own list and release them explicitly before calling bun_destroy().
///
/// AVOID DOUBLE-WRAP
///   Wrapping the same `data` pointer twice creates two independent finalizers;
///   both will fire and cause a double-free. Each C pointer must be passed to
///   at most one bun_array_buffer() / bun_typed_array() call.
///
/// @param ctx        JS context.
/// @param data       Backing memory (must remain valid until finalizer fires).
/// @param len        Byte length.
/// @param finalizer  Called on GC collection (may be NULL). Must not call Bun
///                   or JS APIs except bun_unprotect() on values previously
///                   bun_protect()'d and released here.
/// @param userdata   Forwarded verbatim to finalizer.
/// @return           BunValue holding the JS ArrayBuffer, or BUN_UNDEFINED on failure.
///                   On failure the finalizer is NOT called — the caller retains ownership.
BunValue bun_array_buffer(BunContext* ctx, void* data, size_t len,
    BunFinalizerFn finalizer, void* userdata);

/// Wrap C memory as a JS TypedArray view (zero-copy).
///
/// Equivalent to `new Float32Array(arraybuffer)` but zero-copy — the view
/// directly references the C pointer. finalizer(userdata) fires when the
/// underlying ArrayBuffer is GC'd (which may be later than the TypedArray
/// itself if JS code has taken a reference to `.buffer`).
///
/// The same MEMORY LIFETIME CONTRACT, LEAK RISK, and AVOID DOUBLE-WRAP rules
/// from bun_array_buffer() apply here.
///
/// CONCURRENT ACCESS
///   Do NOT write to `data` while bun_run_pending_jobs() may be executing;
///   there is no internal lock. Synchronize access at the application level.
///
/// @param ctx           JS context.
/// @param kind          Element type (BUN_FLOAT32_ARRAY, BUN_INT32_ARRAY, …).
/// @param data          Backing memory.
/// @param element_count Number of *elements* (not bytes). A size_t overflow
///                      check is performed; the call returns BUN_UNDEFINED and
///                      does NOT invoke the finalizer if it would overflow.
/// @param finalizer     Called on GC collection (may be NULL). Must not call
///                      Bun or JS APIs except bun_unprotect() on values
///                      previously bun_protect()'d and released here.
/// @param userdata      Forwarded verbatim to finalizer.
/// @return              BunValue holding the TypedArray, or BUN_UNDEFINED on failure.
///                      On failure the finalizer is NOT called — caller retains ownership.
BunValue bun_typed_array(BunContext* ctx, BunTypedArrayKind kind,
    void* data, size_t element_count,
    BunFinalizerFn finalizer, void* userdata);

/// Read the backing pointer and length from an ArrayBuffer.
///
/// This performs no coercion. Returns 1 only when `value` is an attached
/// ArrayBuffer and `out` is non-NULL.
int bun_get_array_buffer(BunContext* ctx, BunValue value, BunArrayBufferInfo* out);

/// Read the backing pointer and metadata from a TypedArray.
///
/// This performs no coercion. Returns 1 only when `value` is an attached
/// TypedArray (including subclasses such as Buffer) and `out` is non-NULL.
/// DataView is not accepted.
int bun_get_typed_array(BunContext* ctx, BunValue value, BunTypedArrayInfo* out);

// --------------------------------------------------------------------------
// Class API
// --------------------------------------------------------------------------

/// Register a host-defined class for this runtime.
///
/// The returned BunClass* is only valid for the current runtime. Pass a parent
/// returned by a prior bun_class_register() call on the same runtime to build a
/// prototype chain.
BunClass* bun_class_register(BunContext* ctx, const BunClassDescriptor* descriptor, BunClass* parent);

/// Create an instance of a registered BunClass.
///
/// The returned object is a normal BunValue object and can be used with the
/// existing generic APIs (bun_get, bun_set, bun_call, bun_protect, ...).
BunValue bun_class_new(BunContext* ctx, BunClass* klass, void* native_ptr,
    BunClassFinalizerFn finalizer, void* userdata);

/// Return the native pointer for a class instance, or NULL on mismatch.
///
/// `klass` may be any ancestor class handle returned by bun_class_register().
void* bun_class_unwrap(BunContext* ctx, BunValue value, BunClass* klass);

/// Return 1 when value is any BunClass instance, else 0.
int bun_is_class_instance(BunContext* ctx, BunValue value);

/// Return 1 when value is an instance of `klass` or one of its subclasses.
int bun_instanceof_class(BunContext* ctx, BunValue value, BunClass* klass);

/// Dispose an instance early and run its finalizer immediately if needed.
///
/// The finalizer runs at most once. Repeated calls return 0 after the first.
int bun_class_dispose(BunContext* ctx, BunValue value);

/// Return the runtime-local prototype object for a registered class.
BunValue bun_class_prototype(BunContext* ctx, BunClass* klass);

/// Return the runtime-local constructor function for a registered class.
///
/// Returns BUN_UNDEFINED when the class was registered without a constructor
/// callback.
BunValue bun_class_constructor(BunContext* ctx, BunClass* klass);

// --------------------------------------------------------------------------
// Value Introspection & Conversion
// --------------------------------------------------------------------------

int bun_is_undefined(BunValue value);
int bun_is_null(BunValue value);
int bun_is_bool(BunValue value);
int bun_is_number(BunValue value);
int bun_is_string(BunValue value);
int bun_is_object(BunValue value);

/// Return 1 when value is a JavaScript Array (including subclasses).
/// TypedArrays (Uint8Array, Float32Array, etc.) are NOT arrays and return 0.
/// Use bun_get_typed_array() to test for TypedArrays.
int bun_is_array(BunValue value);
int bun_is_callable(BunValue value);

int bun_to_bool(BunValue value);
double bun_to_number(BunContext* ctx, BunValue value);
int32_t bun_to_int32(BunValue value);

/// Returns a newly allocated UTF-8 string. Caller must free() the returned pointer.
/// On failure, returns NULL.
char* bun_to_utf8(BunContext* ctx, BunValue value, size_t* out_len);

/// Get the "length" of any value that has a length-like property.
///
/// Works for Array, TypedArray (element count), String, ArrayBuffer (byteLength),
/// Map/Set (size), and any object with a numeric .length property.
/// TypedArrays ARE supported — returns the element count, not byte length.
/// Returns -1 if the value has no applicable length.
int64_t bun_array_length(BunContext* ctx, BunValue value);

// --------------------------------------------------------------------------
// Object & Property Operations
// --------------------------------------------------------------------------

int bun_set(BunContext* ctx, BunValue object, const char* key, size_t key_len, BunValue value);
BunValue bun_get(BunContext* ctx, BunValue object, const char* key, size_t key_len);

int bun_set_index(BunContext* ctx, BunValue object, uint32_t index, BunValue value);
BunValue bun_get_index(BunContext* ctx, BunValue object, uint32_t index);

/// Read `count` consecutive elements from a JavaScript Array.
///
/// Returns 1 on success, 0 on failure. On failure, bun_last_error() may
/// describe the reason. `array` must satisfy bun_is_array(). When `count == 0`,
/// this succeeds and `out_values` may be NULL.
int bun_array_get_range(BunContext* ctx, BunValue array, uint32_t start, uint32_t count, BunValue* out_values);

/// Write `count` consecutive elements into a JavaScript Array.
///
/// Returns 1 on success, 0 on failure. On failure, bun_last_error() may
/// describe the reason. `array` must satisfy bun_is_array(). Phase 1 does not
/// grow arrays, so `start + count` must fit within the current array length.
/// When `count == 0`, this succeeds and `values` may be NULL.
int bun_array_set_range(BunContext* ctx, BunValue array, uint32_t start, uint32_t count, const BunValue* values);

/// Define or replace only the getter half of an accessor property.
///
/// Getter-only properties are allowed. Repeated calls replace only the getter
/// callback and getter userdata; any existing setter and setter userdata are
/// preserved.

int bun_define_getter(
    BunContext* ctx,
    BunValue object,
    const char* key,
    size_t key_len,
    BunGetterFn getter,
    void* userdata,
    int dont_enum,
    int dont_delete);

/// Define or replace only the setter half of an accessor property.
///
/// Setter-only properties are allowed. If no getter is defined, reads return
/// `undefined`. Repeated calls replace only the setter callback and setter
/// userdata; any existing getter and getter userdata are preserved.

int bun_define_setter(
    BunContext* ctx,
    BunValue object,
    const char* key,
    size_t key_len,
    BunSetterFn setter,
    void* userdata,
    int dont_enum,
    int dont_delete);

/// Define or replace an accessor property.
///
/// `getter` and `setter` may be provided independently so long as at least one
/// callback is non-NULL. The same `userdata` pointer is passed to both sides.
/// Use bun_define_getter() / bun_define_setter() when getter and setter should
/// keep separate userdata or be updated independently.

int bun_define_accessor(
    BunContext* ctx,
    BunValue object,
    const char* key,
    size_t key_len,
    BunGetterFn getter,
    BunSetterFn setter,
    void* userdata,
    int read_only,
    int dont_enum,
    int dont_delete);

/// Attach a GC finalizer to a JavaScript object.
///
/// Each object may have at most one user finalizer attached through this API.
/// Repeated calls for the same object return 0 and leave the existing
/// finalizer unchanged; they do not replace or stack finalizers.
///
/// If the underlying attach operation fails, no finalizer is recorded and the
/// caller may retry.
int bun_define_finalizer(
    BunContext* ctx,
    BunValue object,
    BunFinalizerFn finalizer,
    void* userdata);

int bun_set_prototype(BunContext* ctx, BunValue object, BunValue proto);

void bun_set_opaque(BunContext* ctx, BunValue object, void* opaque_ptr);
void* bun_get_opaque(BunContext* ctx, BunValue object);

// --------------------------------------------------------------------------
// Function Call & GC Lifetime
// --------------------------------------------------------------------------

/// Call a JavaScript function synchronously.
///
/// @param ctx        JS context.
/// @param fn         Callable BunValue.
/// @param this_value The 'this' binding. Pass BUN_UNDEFINED for global 'this'.
/// @param argc       Argument count.
/// @param argv       Argument array (may be NULL when argc == 0).
/// @return           The JS return value on success, or BUN_EXCEPTION (0) if a
///                   JS exception was thrown. Call bun_last_error(ctx, NULL) to
///                   read the exception message after a BUN_EXCEPTION return.
BunValue bun_call(BunContext* ctx, BunValue fn, BunValue this_value, int argc, const BunValue* argv);

/// Return the error string from the most recent bun_call()/bun_eval*() that
/// failed on this context. Owned by the runtime; valid until the next
/// bun_call()/bun_eval*() call. Returns NULL if no error is pending.
///
/// @param ctx      JS context.
/// @param out_len  Optional output for error length in bytes, excluding the
///                 null terminator. Set to 0 when no error is pending.
const char* bun_last_error(BunContext* ctx, size_t* out_len);

/// Queue a JavaScript function call to run on the context's owning runtime.
///
/// The call is executed later when the host drives bun_run_pending_jobs().
/// On success this also wakes the runtime's event loop automatically, so an
/// additional bun_wakeup() call is usually unnecessary.
/// Returns 1 if queued successfully, 0 on failure.
int bun_call_async(BunContext* ctx, BunValue fn, BunValue this_value, int argc, const BunValue* argv);

void bun_protect(BunContext* ctx, BunValue value);
void bun_unprotect(BunContext* ctx, BunValue value);

#ifdef __cplusplus
}
#endif

#endif // BUN_EMBED_H
