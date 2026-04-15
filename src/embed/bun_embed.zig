///! bun_embed.zig — High-performance C API for embedding Bun's JS runtime.
///! See bun_embed.h for API documentation.
const std = @import("std");
const bun = @import("bun");
const jsc = bun.jsc;
const js_ast = bun.ast;
const Arena = bun.allocators.MimallocArena;
const VirtualMachine = jsc.VirtualMachine;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const Environment = bun.Environment;
const api = bun.schema.api;

const BunValue = u64;
const BunContext = opaque {};
const BunClass = opaque {};

const BunHostFn = *const fn (?*BunContext, c_int, ?[*]const BunValue, ?*anyopaque) callconv(.c) BunValue;
const BunGetterFn = *const fn (?*BunContext, BunValue, ?*anyopaque) callconv(.c) BunValue;
const BunSetterFn = *const fn (?*BunContext, BunValue, BunValue, ?*anyopaque) callconv(.c) void;
const BunFinalizerFn = *const fn (?*anyopaque) callconv(.c) void;
const BunClassMethodFn = *const fn (?*BunContext, BunValue, ?*anyopaque, c_int, ?[*]const BunValue, ?*anyopaque) callconv(.c) BunValue;
const BunClassGetterFn = *const fn (?*BunContext, BunValue, ?*anyopaque, ?*anyopaque) callconv(.c) BunValue;
const BunClassSetterFn = *const fn (?*BunContext, BunValue, ?*anyopaque, BunValue, ?*anyopaque) callconv(.c) void;
const BunClassConstructorFn = *const fn (?*BunContext, ?*BunClass, c_int, ?[*]const BunValue, ?*anyopaque) callconv(.c) BunValue;
const BunClassStaticMethodFn = *const fn (?*BunContext, BunValue, ?*anyopaque, c_int, ?[*]const BunValue) callconv(.c) BunValue;
const BunClassStaticGetterFn = *const fn (?*BunContext, BunValue, ?*anyopaque) callconv(.c) BunValue;
const BunClassStaticSetterFn = *const fn (?*BunContext, BunValue, BunValue, ?*anyopaque) callconv(.c) void;
const BunClassFinalizerFn = *const fn (?*anyopaque, ?*anyopaque) callconv(.c) void;

const BunClassMethodDescriptor = extern struct {
    name: ?[*]const u8,
    name_len: usize,
    callback: ?BunClassMethodFn,
    userdata: ?*anyopaque,
    arg_count: c_int,
    dont_enum: c_int,
    dont_delete: c_int,
};

const BunClassPropertyDescriptor = extern struct {
    name: ?[*]const u8,
    name_len: usize,
    getter: ?BunClassGetterFn,
    setter: ?BunClassSetterFn,
    userdata: ?*anyopaque,
    read_only: c_int,
    dont_enum: c_int,
    dont_delete: c_int,
};

const BunClassStaticMethodDescriptor = extern struct {
    name: ?[*]const u8,
    name_len: usize,
    callback: ?BunClassStaticMethodFn,
    userdata: ?*anyopaque,
    arg_count: c_int,
    dont_enum: c_int,
    dont_delete: c_int,
};

const BunClassStaticPropertyDescriptor = extern struct {
    name: ?[*]const u8,
    name_len: usize,
    getter: ?BunClassStaticGetterFn,
    setter: ?BunClassStaticSetterFn,
    userdata: ?*anyopaque,
    read_only: c_int,
    dont_enum: c_int,
    dont_delete: c_int,
};

const BunClassDescriptor = extern struct {
    name: ?[*]const u8,
    name_len: usize,
    properties: ?[*]const BunClassPropertyDescriptor,
    property_count: usize,
    methods: ?[*]const BunClassMethodDescriptor,
    method_count: usize,
    constructor: ?BunClassConstructorFn,
    constructor_userdata: ?*anyopaque,
    constructor_arg_count: c_int,
    static_properties: ?[*]const BunClassStaticPropertyDescriptor,
    static_property_count: usize,
    static_methods: ?[*]const BunClassStaticMethodDescriptor,
    static_method_count: usize,
};

const BunArrayBufferInfo = extern struct {
    data: ?*anyopaque,
    byte_length: usize,
};

const BunTypedArrayInfo = extern struct {
    data: ?*anyopaque,
    byte_offset: usize,
    byte_length: usize,
    element_count: usize,
    kind: u32,
};

const BunPendingJobsResult = enum(c_int) {
    idle = 0,
    spin = 1,
    wait = 2,
};

const HostFnData = struct {
    native_fn: BunHostFn,
    userdata: ?*anyopaque,
    /// Back-pointer so the GC finalizer can remove this entry from the registry.
    runtime: *BunRuntime,
    /// The JSValue key used in host_fn_registry (needed for removal in finalizer).
    js_fn: JSValue,
};

/// Per-object side-table entry for opaque pointer + finalizer-dedup guard.
const OpaqueEntry = struct {
    /// Native pointer stored by bun_set_opaque().
    opaque_ptr: ?*anyopaque = null,
    /// Whether a user finalizer has already been attached via bun_define_finalizer().
    /// This is only set after the underlying attach succeeds so callers may retry
    /// after an attach failure.
    finalizer_attached: bool = false,
};

const PendingCall = struct {
    fn_value: BunValue,
    this_value: BunValue,
    argv: []BunValue,
};

// ---------------------------------------------------------------------------
// Futex-based ACK primitive for the event watcher background thread.
// Uses a single std.atomic.Value(u32) + std.Thread.Futex across all
// platforms (Linux: futex(), macOS: __ulock_wait2(), Windows:
// RtlWaitOnAddress()). This replaces the previous platform-split approach
// (POSIX std.Thread.Semaphore / Windows CreateEventW) with a unified
// single-syscall mechanism.
// ---------------------------------------------------------------------------
const Futex = std.Thread.Futex;

/// Opaque runtime handle exposed to C as `BunRuntime*`.
const BunRuntime = struct {
    vm: *VirtualMachine,
    arena: Arena,
    /// Scratch buffer for returning error strings to C callers. Valid until next eval call.
    last_error_buf: ?[*:0]u8 = null,
    pending_calls: std.ArrayListUnmanaged(PendingCall) = .{},
    pending_calls_mutex: std.Thread.Mutex = .{},
    /// Per-runtime registry of JSValue → HostFnData. Replaces the old global map.
    host_fn_registry: HostFnMap = .{},
    /// Per-runtime side-table for opaque pointers and finalizer-dedup guards.
    /// Keyed by JSValue identity; replaces the old string-property approach.
    opaque_map: OpaqueMap = .{},
    /// Runtime-local class handles allocated by BunEmbed.cpp.
    class_registry: std.ArrayListUnmanaged(*BunClass) = .{},

    // -----------------------------------------------------------------------
    // Event-watcher fields (populated by bun_set_event_callback)
    // -----------------------------------------------------------------------

    /// Host callback invoked from the background watcher thread when the JS
    /// event loop transitions from idle to having ready work.
    event_callback_fn: ?*const fn (?*anyopaque) callconv(.c) void = null,
    event_callback_userdata: ?*anyopaque = null,
    /// Background thread that monitors the platform event fd / timer deadline.
    event_watcher_thread: ?std.Thread = null,
    /// Set to true to ask the watcher thread to exit.
    event_watcher_stop: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    /// Futex-based ACK primitive — unified across all platforms.
    /// 0 = watcher is waiting (or will wait soon); 1 = ACK posted.
    /// Watcher: stores 0, then Futex.wait(&ack, 0) blocks until value != 0.
    /// Host:    stores 1, then Futex.wake(&ack, 1) unblocks the watcher.
    /// Single kernel syscall per platform (futex / __ulock / RtlWaitOnAddress).
    event_watcher_ack: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),
    /// Set to true by the watcher thread after it has fired the callback and
    /// is waiting for ACK. Checked by bun_run_pending_jobs() to avoid posting
    /// a stale ACK when the watcher hasn't actually notified the host yet.
    event_watcher_needs_ack: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),

    fn setLastErrorBytes(runtime: *BunRuntime, bytes: []const u8) void {
        const err_str = bun.default_allocator.allocSentinel(u8, bytes.len, 0) catch return;
        if (bytes.len > 0) {
            @memcpy(err_str[0..bytes.len], bytes);
        }

        runtime.freeLastError();
        runtime.last_error_buf = err_str;
    }

    fn captureException(runtime: *BunRuntime, global: *JSGlobalObject, value: JSValue) void {
        var thrown_value = value;

        if (!thrown_value.isAnyError()) {
            if (thrown_value.toError()) |error_like| {
                thrown_value = error_like;
            } else if (thrown_value.asException(global.vm())) |exception| {
                thrown_value = exception.value();
                if (!thrown_value.isAnyError()) {
                    if (thrown_value.toError()) |error_like| {
                        thrown_value = error_like;
                    }
                }
            }
        }

        if (!thrown_value.isAnyError() and thrown_value.isObject()) {
            const maybe_name_value = (thrown_value.getOwn(global, "name") catch null) orelse
                (thrown_value.getPropertyValue(global, "name") catch null);
            const maybe_message_value = (thrown_value.getOwn(global, "message") catch null) orelse
                (thrown_value.getPropertyValue(global, "message") catch null);

            if (maybe_message_value) |message_value| {
                if (message_value.isString()) {
                    const message_bytes = message_value.toUTF8Bytes(global, bun.default_allocator) catch null;
                    if (message_bytes) |message| {
                        defer bun.default_allocator.free(message);

                        if (maybe_name_value) |name_value| {
                            if (name_value.isString()) {
                                const name_bytes = name_value.toUTF8Bytes(global, bun.default_allocator) catch null;
                                if (name_bytes) |name| {
                                    defer bun.default_allocator.free(name);

                                    const combined = std.fmt.allocPrint(bun.default_allocator, "{s}: {s}", .{ name, message }) catch null;
                                    if (combined) |text| {
                                        defer bun.default_allocator.free(text);
                                        runtime.setLastErrorBytes(text);
                                        return;
                                    }
                                }
                            }
                        }

                        runtime.setLastErrorBytes(message);
                        return;
                    }
                }
            }
        }

        // Non-Error thrown values are reported with side-effect-free, type-based
        // messages. This avoids invoking coercion hooks like Symbol.toPrimitive
        // while trying to format the exception itself.
        if (!thrown_value.isAnyError()) {
            if (thrown_value.isUndefined()) {
                runtime.setLastErrorBytes("thrown value: undefined");
            } else if (thrown_value.isNull()) {
                runtime.setLastErrorBytes("thrown value: null");
            } else if (thrown_value.isBoolean()) {
                runtime.setLastErrorBytes(if (thrown_value.asBoolean()) "thrown value: true" else "thrown value: false");
            } else if (thrown_value.isNumber()) {
                runtime.setLastErrorBytes("thrown value: number");
            } else if (thrown_value.isString()) {
                runtime.setLastErrorBytes("thrown value: string");
            } else if (thrown_value.isBigInt()) {
                runtime.setLastErrorBytes("thrown value: bigint");
            } else if (thrown_value.isSymbol()) {
                runtime.setLastErrorBytes("thrown value: symbol");
            } else if (thrown_value.isObject()) {
                runtime.setLastErrorBytes("thrown value: object");
            } else {
                runtime.setLastErrorBytes("thrown value: unknown");
            }
            return;
        }

        var array = std.Io.Writer.Allocating.init(bun.default_allocator);
        defer array.deinit();

        jsc.ConsoleObject.format2(.Error, global, @ptrCast(&thrown_value), 1, &array.writer, .{
            .enable_colors = false,
            .add_newline = false,
            .flush = false,
            .quote_strings = true,
            .ordered_properties = false,
            .max_depth = 4,
        }) catch {
            global.clearException();
            runtime.setLastErrorBytes("error: [failed to format error]");
            return;
        };

        if (global.hasException()) {
            global.clearException();
            runtime.setLastErrorBytes("error: [failed to format error]");
            return;
        }

        array.writer.flush() catch {
            runtime.setLastErrorBytes("exception (failed to capture message)");
            return;
        };

        runtime.setLastErrorBytes(array.written());
    }

    fn freeLastError(runtime: *BunRuntime) void {
        if (runtime.last_error_buf) |buf| {
            bun.default_allocator.free(std.mem.span(buf));
            runtime.last_error_buf = null;
        }
    }

    fn freePendingCalls(runtime: *BunRuntime) void {
        runtime.pending_calls_mutex.lock();
        defer runtime.pending_calls_mutex.unlock();

        for (runtime.pending_calls.items) |call| {
            bun.default_allocator.free(call.argv);
        }
        runtime.pending_calls.deinit(bun.default_allocator);
    }

    fn freeHostFnRegistry(runtime: *BunRuntime) void {
        var it = runtime.host_fn_registry.valueIterator();
        while (it.next()) |item| {
            bun.default_allocator.destroy(item.*);
        }
        runtime.host_fn_registry.deinit(bun.default_allocator);
    }

    fn freeOpaqueMap(runtime: *BunRuntime) void {
        runtime.opaque_map.deinit(bun.default_allocator);
    }

    fn freeClassRegistry(runtime: *BunRuntime) void {
        for (runtime.class_registry.items) |class_handle| {
            BunEmbed__destroyClass(class_handle);
        }
        runtime.class_registry.deinit(bun.default_allocator);
    }
};

const BunDebuggerMode = enum(c_int) {
    off = 0,
    attach = 1,
    wait = 2,
    @"break" = 3,
};

const BunInitializeOptions = extern struct {
    cwd: ?[*:0]const u8,
    debugger_mode: BunDebuggerMode,
    debugger_listen_url: ?[*:0]const u8,
};

const HostFnMap = std.AutoHashMapUnmanaged(JSValue, *HostFnData);
const OpaqueMap = std.AutoHashMapUnmanaged(JSValue, OpaqueEntry);

/// Global lookup table: VirtualMachine* → *BunRuntime.
/// Populated in bun_initialize(), removed in bun_destroy().
/// This is the only remaining global state, and it is necessary because the
/// hostFnTrampoline's JSC callback only receives the JSGlobalObject; we need
/// to recover the owning BunRuntime to access its per-runtime registries.
var vm_to_runtime_map: std.AutoHashMapUnmanaged(*VirtualMachine, *BunRuntime) = .{};
var vm_to_runtime_mutex: std.Thread.Mutex = .{};

fn vmToRuntime(vm: *VirtualMachine) ?*BunRuntime {
    vm_to_runtime_mutex.lock();
    defer vm_to_runtime_mutex.unlock();
    return vm_to_runtime_map.get(vm);
}

fn registerRuntime(runtime: *BunRuntime) void {
    vm_to_runtime_mutex.lock();
    defer vm_to_runtime_mutex.unlock();
    vm_to_runtime_map.put(bun.default_allocator, runtime.vm, runtime) catch {};
}

fn unregisterRuntime(runtime: *BunRuntime) void {
    vm_to_runtime_mutex.lock();
    defer vm_to_runtime_mutex.unlock();
    _ = vm_to_runtime_map.remove(runtime.vm);
}

fn toBunValue(value: JSValue) BunValue {
    return @as(BunValue, @bitCast(@as(i64, @intFromEnum(value))));
}

fn toJSValue(value: BunValue) JSValue {
    return @as(JSValue, @enumFromInt(@as(i64, @bitCast(value))));
}

fn toGlobal(ctx: ?*BunContext) ?*JSGlobalObject {
    return if (ctx) |ptr| @ptrCast(ptr) else null;
}

extern fn BunString__createUTF8ForJS(globalObject: *JSGlobalObject, ptr: [*]const u8, length: usize) JSValue;
extern fn Bun__REPL__evaluate(
    globalObject: *JSGlobalObject,
    sourcePtr: [*]const u8,
    sourceLen: usize,
    filenamePtr: [*]const u8,
    filenameLen: usize,
    exception: *JSValue,
) JSValue;

extern fn JSFunction__createFromZig(
    global: *JSGlobalObject,
    fn_name: bun.String,
    implementation: *const jsc.JSHostFn,
    arg_count: u32,
    implementation_visibility: u8,
    intrinsic: u8,
    constructor: ?*const jsc.JSHostFn,
) JSValue;

extern fn BunEmbed__defineCustomAccessor(
    global: *JSGlobalObject,
    object: JSValue,
    key_ptr: [*]const u8,
    key_len: usize,
    getter: ?BunGetterFn,
    getter_userdata: ?*anyopaque,
    setter: ?BunSetterFn,
    setter_userdata: ?*anyopaque,
    update_mask: u8,
    flags: u32,
) bool;

extern fn BunEmbed__defineFinalizer(
    global: *JSGlobalObject,
    object: JSValue,
    finalizer: BunFinalizerFn,
    userdata: ?*anyopaque,
) bool;

extern fn BunEmbed__createArrayBuffer(
    global: *JSGlobalObject,
    data: ?*anyopaque,
    byte_len: usize,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) JSValue;

extern fn BunEmbed__createTypedArray(
    global: *JSGlobalObject,
    kind: u32,
    data: ?*anyopaque,
    element_count: usize,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) JSValue;

extern fn BunEmbed__getArrayBuffer(
    global: *JSGlobalObject,
    value: JSValue,
    out: *BunArrayBufferInfo,
) bool;

extern fn BunEmbed__getTypedArray(
    global: *JSGlobalObject,
    value: JSValue,
    out: *BunTypedArrayInfo,
) bool;

extern fn BunEmbed__registerClass(
    global: *JSGlobalObject,
    descriptor: *const BunClassDescriptor,
    parent: ?*BunClass,
) ?*BunClass;

extern fn BunEmbed__destroyClass(class_handle: *BunClass) void;

extern fn BunEmbed__createClassInstance(
    global: *JSGlobalObject,
    class_handle: *BunClass,
    native_ptr: ?*anyopaque,
    finalizer: ?BunClassFinalizerFn,
    userdata: ?*anyopaque,
) JSValue;

extern fn BunEmbed__unwrapClassInstance(
    global: *JSGlobalObject,
    value: JSValue,
    class_handle: ?*BunClass,
) ?*anyopaque;

extern fn BunEmbed__isClassInstance(
    global: *JSGlobalObject,
    value: JSValue,
) bool;

extern fn BunEmbed__instanceofClass(
    global: *JSGlobalObject,
    value: JSValue,
    class_handle: *BunClass,
) bool;

extern fn BunEmbed__disposeClassInstance(
    global: *JSGlobalObject,
    value: JSValue,
) bool;

extern fn BunEmbed__classPrototype(
    global: *JSGlobalObject,
    class_handle: *BunClass,
) JSValue;

extern fn BunEmbed__classConstructor(
    global: *JSGlobalObject,
    class_handle: *BunClass,
) JSValue;

extern fn BunEmbed__arrayGetRange(
    global: *JSGlobalObject,
    value: JSValue,
    start: u32,
    count: u32,
    out_values: [*]BunValue,
) bool;

extern fn BunEmbed__arraySetRange(
    global: *JSGlobalObject,
    value: JSValue,
    start: u32,
    count: u32,
    values: [*]const BunValue,
) bool;

const BUN_ACCESSOR_READ_ONLY: u32 = 1 << 0;
const BUN_ACCESSOR_DONT_ENUM: u32 = 1 << 1;
const BUN_ACCESSOR_DONT_DELETE: u32 = 1 << 2;
const BUN_ACCESSOR_UPDATE_GETTER: u8 = 1 << 0;
const BUN_ACCESSOR_UPDATE_SETTER: u8 = 1 << 1;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

pub export fn bun_initialize(options: ?*const BunInitializeOptions) callconv(.c) ?*BunRuntime {
    return initializeImpl(options) catch null;
}

fn debuggerFromOptions(options: ?*const BunInitializeOptions) bun.cli.Command.Debugger {
    const opts = options orelse return .{ .unspecified = {} };
    const path_or_port = if (opts.debugger_listen_url) |value| std.mem.span(value) else "";

    return switch (opts.debugger_mode) {
        .off => .{ .unspecified = {} },
        .attach => .{ .enable = .{ .path_or_port = path_or_port } },
        .wait => .{ .enable = .{ .path_or_port = path_or_port, .wait_for_connection = true } },
        .@"break" => .{ .enable = .{
            .path_or_port = path_or_port,
            .wait_for_connection = true,
            .set_breakpoint_on_first_line = true,
        } },
    };
}

fn initializeEmbedOutput() void {
    const stdout = bun.sys.File.from(std.fs.File.stdout());
    const stderr = bun.sys.File.from(std.fs.File.stderr());
    bun.Output.Source.setInit(stdout, stderr);
}

fn initializeImpl(options: ?*const BunInitializeOptions) !?*BunRuntime {
    // Crash handler (idempotent)
    bun.crash_handler.init();

    if (Environment.isPosix) {
        var act: std.posix.Sigaction = .{
            .handler = .{ .handler = std.posix.SIG.IGN },
            .mask = std.posix.sigemptyset(),
            .flags = 0,
        };
        std.posix.sigaction(std.posix.SIG.PIPE, &act, null);
    }

    // Initialize JSC
    bun.jsc.initialize(false);

    // AST stores
    js_ast.Expr.Data.Store.create();
    js_ast.Stmt.Data.Store.create();

    // Arena
    const arena = Arena.init();
    const allocator = arena.allocator();

    // Build a minimal TransformOptions
    var args = api.TransformOptions{
        .entry_points = &.{},
        .inject = &.{},
        .external = &.{},
        .main_fields = &.{},
        .env_files = &.{},
        .extension_order = &.{},
        .conditions = &.{},
        .ignore_dce_annotations = false,
        .bunfig_path = "",
    };

    // Set the working directory if provided
    if (options) |opts| if (opts.cwd) |cwd| {
        args.absolute_working_dir = std.mem.span(cwd);
    };

    // Bind Bun's output streams to the host process stdio without running the
    // CLI stdio bootstrap, which would mutate global process state.
    initializeEmbedOutput();

    // Create the VM
    const vm = try VirtualMachine.init(.{
        .allocator = allocator,
        .args = args,
        .debugger = debuggerFromOptions(options),
        .is_main_thread = true,
    });

    var b = &vm.transpiler;
    b.options.env.behavior = .load_all_without_inlining;

    b.configureDefines() catch {
        return null;
    };

    vm.loadExtraEnvAndSourceCodePrinter();
    vm.is_main_thread = true;
    jsc.VirtualMachine.is_main_thread_vm = true;

    // Allocate the runtime handle
    const rt = bun.default_allocator.create(BunRuntime) catch return null;
    rt.* = .{
        .vm = vm,
        .arena = arena,
        .last_error_buf = null,
    };

    registerRuntime(rt);
    return rt;
}

pub export fn bun_destroy(rt: ?*BunRuntime) callconv(.c) void {
    const runtime = rt orelse return;
    stopEventWatcher(runtime);
    unregisterRuntime(runtime);
    runtime.freeLastError();
    runtime.freePendingCalls();
    runtime.freeHostFnRegistry();
    runtime.freeOpaqueMap();
    runtime.freeClassRegistry();

    runtime.vm.onExit();
    bun.default_allocator.destroy(runtime);
}

pub export fn bun_context(rt: ?*BunRuntime) callconv(.c) ?*BunContext {
    const runtime = rt orelse return null;
    return @ptrCast(runtime.vm.global);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

pub export fn bun_eval_string(ctx: ?*BunContext, code_ptr: ?[*]const u8, code_len: usize) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return 0;
    const runtime = vmToRuntime(global.bunVM()) orelse return 0;
    runtime.freeLastError();

    const code = if (code_ptr) |p| p[0..code_len] else {
        runtime.setLastErrorBytes("null code");
        return 0;
    };

    var eval_ctx = EvalContext{
        .runtime = runtime,
        .global = global,
        .code = code,
        .result = 0,
    };
    runtime.vm.runWithAPILock(EvalContext, &eval_ctx, EvalContext.run);
    return eval_ctx.result;
}

const EvalContext = struct {
    runtime: *BunRuntime,
    global: *JSGlobalObject,
    code: []const u8,
    result: BunValue,

    pub fn run(this: *EvalContext) void {
        // Lazily start the inspector thread on first eval (idempotent).
        this.runtime.vm.ensureDebugger(false) catch {};

        var exception: JSValue = .js_undefined;
        const ret = Bun__REPL__evaluate(
            this.global,
            this.code.ptr,
            this.code.len,
            "embed:eval",
            "embed:eval".len,
            &exception,
        );

        var final_result = ret;

        if (exception != .js_undefined and exception != .zero) {
            this.runtime.captureException(this.global, exception);
            return;
        }

        if (ret.asAnyPromise()) |promise| {
            promise.setHandled(this.global.vm());
            this.runtime.vm.waitForPromise(promise);

            switch (promise.status()) {
                .fulfilled => {
                    final_result = promise.result(this.global.vm());
                },
                .rejected => {
                    const rejection = promise.result(this.global.vm());
                    this.runtime.captureException(this.global, rejection);
                    return;
                },
                .pending => {
                    this.runtime.setLastErrorBytes("evaluation promise did not settle");
                    return;
                },
            }
        }

        if (this.global.tryTakeException()) |exc| {
            this.runtime.captureException(this.global, exc);
        } else if (final_result == .zero) {
            this.runtime.setLastErrorBytes("evaluation returned null");
        } else {
            this.result = toBunValue(final_result);
        }
    }
};

pub export fn bun_eval_file(ctx: ?*BunContext, path_ptr: ?[*]const u8, path_len: usize) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return 0;
    const runtime = vmToRuntime(global.bunVM()) orelse return 0;
    runtime.freeLastError();

    const path = if (path_ptr) |p| p[0..path_len] else {
        runtime.setLastErrorBytes("null path");
        return 0;
    };

    if (std.mem.indexOfScalar(u8, path, 0) != null) {
        runtime.setLastErrorBytes("path contains embedded NUL");
        return 0;
    }

    var eval_ctx = EvalFileContext{
        .runtime = runtime,
        .global = global,
        .path = path,
        .result = 0,
    };
    runtime.vm.runWithAPILock(EvalFileContext, &eval_ctx, EvalFileContext.run);
    return eval_ctx.result;
}

const EvalFileContext = struct {
    runtime: *BunRuntime,
    global: *JSGlobalObject,
    path: []const u8,
    result: BunValue,

    pub fn run(this: *EvalFileContext) void {
        const vm = this.runtime.vm;

        // Clear any previously loaded entry point from the module registry so
        // that bun_eval_file can be called more than once on the same runtime.
        vm.clearEntryPoint() catch {
            if (this.global.tryTakeException()) |exc| {
                this.runtime.captureException(this.global, exc);
            } else {
                this.runtime.setLastErrorBytes("failed to clear previous entry point");
            }
            return;
        };

        const promise = vm.loadEntryPoint(this.path) catch {
            if (this.global.tryTakeException()) |exc| {
                this.runtime.captureException(this.global, exc);
            } else {
                this.runtime.setLastErrorBytes("failed to load entry point");
            }
            return;
        };

        switch (promise.status()) {
            .pending => {
                promise.setHandled(this.global.vm());
                vm.waitForPromise(.{ .internal = promise });

                switch (promise.status()) {
                    .fulfilled => {
                        this.result = toBunValue(.js_undefined);
                    },
                    .rejected => {
                        const rejection = promise.result();
                        this.runtime.captureException(this.global, rejection);
                    },
                    .pending => {
                        this.runtime.setLastErrorBytes("entry point promise did not settle");
                    },
                }
            },
            .rejected => {
                const rejection = promise.result();
                this.runtime.captureException(this.global, rejection);
            },
            .fulfilled => {
                this.result = toBunValue(.js_undefined);
            },
        }
    }
};

// ---------------------------------------------------------------------------
// Event Loop Integration
// ---------------------------------------------------------------------------

pub export fn bun_run_pending_jobs(rt: ?*BunRuntime) callconv(.c) BunPendingJobsResult {
    const runtime = rt orelse return .idle;
    var tick_ctx = TickContext{ .runtime = runtime, .result = .idle };
    runtime.vm.runWithAPILock(TickContext, &tick_ctx, TickContext.run);
    // Signal the watcher thread that we have finished processing so it can
    // safely resume its blocking wait (poll on POSIX, IOCP on Windows).
    // Only post ACK if the watcher has actually notified us and is waiting;
    // this prevents stale ACKs from accumulating.
    if (runtime.event_watcher_thread != null and
        !runtime.event_watcher_stop.load(.acquire) and
        runtime.event_watcher_needs_ack.swap(false, .acq_rel))
    {
        runtime.event_watcher_ack.store(1, .release);
        Futex.wake(&runtime.event_watcher_ack, 1);
    }
    return tick_ctx.result;
}

const TickContext = struct {
    runtime: *BunRuntime,
    result: BunPendingJobsResult,

    fn hasQueuedPendingCalls(runtime: *BunRuntime) bool {
        runtime.pending_calls_mutex.lock();
        defer runtime.pending_calls_mutex.unlock();
        return runtime.pending_calls.items.len > 0;
    }

    fn hasDueTimerNow(vm: *VirtualMachine) bool {
        const timers = &vm.timer;
        timers.lock.lock();
        defer timers.lock.unlock();

        const timer = timers.timers.peek() orelse return false;
        const now = bun.timespec.now(.allow_mocked_time);
        return !timer.next.greater(&now);
    }

    fn hasImmediateProgressAvailable(runtime: *BunRuntime) bool {
        const vm = runtime.vm;
        const event_loop = vm.eventLoop();

        if (hasQueuedPendingCalls(runtime)) return true;
        if (vm.pending_unref_counter > 0) return true;
        if (vm.after_event_loop_callback != null) return true;
        if (event_loop.tasks.count > 0) return true;
        if (event_loop.immediate_tasks.items.len > 0) return true;
        if (event_loop.next_immediate_tasks.items.len > 0) return true;
        if (event_loop.deferred_tasks.map.count() > 0) return true;
        if (!event_loop.concurrent_tasks.isEmpty()) return true;
        if (hasDueTimerNow(vm)) return true;

        if (vm.event_loop_handle) |loop| {
            if (comptime Environment.isPosix) {
                var pfd = [1]std.posix.pollfd{.{
                    .fd = loop.fd,
                    .events = std.posix.POLL.IN,
                    .revents = 0,
                }};
                return (std.posix.poll(&pfd, 0) catch 0) > 0;
            }

            return bun.windows.libuv.uv_backend_timeout(loop) == 0;
        }

        return false;
    }

    fn hasFuturePendingWork(runtime: *BunRuntime) bool {
        const vm = runtime.vm;
        const event_loop = vm.eventLoop();

        return vm.isEventLoopAlive() or
            !event_loop.concurrent_tasks.isEmpty() or
            vm.after_event_loop_callback != null or
            event_loop.deferred_tasks.map.count() > 0 or
            hasQueuedPendingCalls(runtime);
    }

    /// Compute the wait hint in milliseconds for the embed host or the
    /// internal watcher thread.
    ///   0  = work is runnable right now (immediate tasks, due timers, etc.)
    ///  -1  = no timers pending; block indefinitely on I/O / wakeup
    ///  >0  = milliseconds until the next timer fires
    ///
    /// Thread safety: this function is safe to call from the background watcher
    /// thread. It acquires the timer lock to peek at the heap and does not
    /// mutate state (unlike Timer.All.getTimeout which has side effects).
    fn getWaitHintMs(runtime: *BunRuntime) i64 {
        if (hasImmediateProgressAvailable(runtime)) return 0;

        const vm = runtime.vm;
        if (comptime Environment.isPosix) {
            // Check immediate_tasks first (same as getTimeout, no lock needed).
            if (vm.event_loop.immediate_tasks.items.len > 0) return 0;

            // Peek the timer heap under lock — do NOT use getTimeout() because
            // it has side effects (fires WTFTimer) that are unsafe from the
            // background watcher thread.
            const timers = &vm.timer;
            timers.lock.lock();
            defer timers.lock.unlock();

            const min = timers.timers.peek() orelse return -1;
            const now = bun.timespec.now(.allow_mocked_time);
            if (!min.next.greater(&now)) return 0; // timer already due
            const spec = min.next.duration(&now);
            const ms: i64 = spec.sec * 1000 + @divTrunc(spec.nsec, 1_000_000);
            return if (ms <= 0) 1 else ms;
        } else {
            if (vm.event_loop_handle) |loop| {
                return bun.windows.libuv.uv_backend_timeout(loop);
            }
            return -1;
        }
    }

    fn drainPendingCalls(runtime: *BunRuntime, global: *JSGlobalObject) void {
        runtime.pending_calls_mutex.lock();
        var local_calls = runtime.pending_calls;
        runtime.pending_calls = .{};
        runtime.pending_calls_mutex.unlock();

        defer {
            for (local_calls.items) |call| {
                bun.default_allocator.free(call.argv);
            }
            local_calls.deinit(bun.default_allocator);
        }

        for (local_calls.items) |call| {
            const fn_value = toJSValue(call.fn_value);
            const this_value = toJSValue(call.this_value);
            const args: []const JSValue = if (call.argv.len == 0)
                &.{}
            else
                @as([*]const JSValue, @ptrCast(call.argv.ptr))[0..call.argv.len];

            _ = fn_value.call(global, this_value, args) catch {
                // Clear any pending exception so the event loop stays healthy.
                global.clearException();
            };
        }
    }

    pub fn run(this: *TickContext) void {
        const vm = this.runtime.vm;
        var event_loop = vm.eventLoop();

        drainPendingCalls(this.runtime, vm.global);

        // Match Bun's normal non-blocking progression more closely:
        // immediates -> ready tasks -> zero-timeout I/O -> due timers -> follow-up tasks.
        event_loop.tickImmediateTasks(vm);
        event_loop.tick();

        if (vm.event_loop_handle) |loop| {
            if (comptime Environment.isPosix) {
                const pending_unref = vm.pending_unref_counter;
                if (pending_unref > 0) {
                    vm.pending_unref_counter = 0;
                    loop.unrefCount(pending_unref);
                }
            }

            vm.timer.updateDateHeaderTimerIfNecessary(loop, vm);

            // Non-blocking I/O poll (kqueue/epoll on POSIX, IOCP via libuv on Windows)
            if (comptime Environment.isPosix) {
                loop.tickWithoutIdle();
                vm.timer.drainTimers(vm);
            } else {
                loop.tickWithTimeout(0);
            }
        }

        vm.onAfterEventLoop();

        // Process tasks made ready by I/O, timers, or deferred after-event-loop callbacks.
        event_loop.tick();
        vm.global.handleRejectedPromises();

        this.result = if (hasImmediateProgressAvailable(this.runtime))
            .spin
        else if (hasFuturePendingWork(this.runtime))
            .wait
        else
            .idle;
    }
};

// Returns the underlying kqueue/epoll fd on POSIX so the host can monitor it
// directly (e.g. via poll/select, CFFileDescriptor, or a GSource) and call
// bun_run_pending_jobs() when it becomes readable.
// Returns -1 on Windows (IOCP has no pollable fd) or if unavailable.
// Cross-platform alternative: bun_set_event_callback() works on all
// platforms including Windows without requiring manual fd polling.
pub export fn bun_get_event_fd(rt: ?*BunRuntime) callconv(.c) c_int {
    const runtime = rt orelse return -1;
    if (comptime Environment.isPosix) {
        if (runtime.vm.event_loop_handle) |loop| {
            return loop.fd;
        }
    }
    return -1;
}

/// Returns the recommended wait timeout in milliseconds for the embed host.
///   0  = work is runnable right now; call bun_run_pending_jobs() immediately.
///  -1  = no JS timers pending; wait indefinitely on I/O / bun_wakeup().
///  >0  = milliseconds until the next JS timer fires; use as poll/select timeout.
pub export fn bun_get_wait_hint(rt: ?*BunRuntime) callconv(.c) i64 {
    const runtime = rt orelse return -1;
    return TickContext.getWaitHintMs(runtime);
}

pub export fn bun_wakeup(rt: ?*BunRuntime) callconv(.c) void {
    const runtime = rt orelse return;
    if (runtime.vm.event_loop_handle) |loop| {
        // On POSIX: wakes the kqueue/epoll fd → watcherThread's poll() returns.
        // On Windows: uv_async_send posts a synthetic completion to loop.iocp →
        //             watcherThread's GetQueuedCompletionStatusEx returns immediately.
        // Note: bun_call_async() already calls loop.wakeup() internally after
        // enqueuing the call, so there is no need to call bun_wakeup() after
        // bun_call_async().
        loop.wakeup();
    }
}

// ---------------------------------------------------------------------------
// Event watcher — background thread + bun_set_event_callback
// ---------------------------------------------------------------------------

/// Background thread that monitors the platform event fd (POSIX) or libuv's
/// IOCP handle (Windows), and invokes the user's callback whenever the JS
/// event loop has ready work.
///
/// POSIX (poll + timeout + ACK):
///   1. Compute timeout via getWaitHintMs():
///        0 = work ready now → poll returns immediately
///       -1 = no timers     → block indefinitely on fd
///       >0 = ms to next timer → use as poll timeout
///   2. Block in poll(fd, timeout) — zero CPU while idle. I/O readiness,
///      cross-thread wakeups (loop.wakeup()), and timer expiry (timeout)
///      all terminate the wait.
///   3. Fire user callback → host calls SDL_PushEvent (or equivalent).
///   4. Wait on Futex ACK until bun_run_pending_jobs() has run,
///      preventing redundant callbacks before the host has drained work.
///
/// Windows (IOCP dequeue-requeue):
///   1. Block in GetQueuedCompletionStatusEx(loop.iocp, …) — this is a TRUE
///      blocking wait: TCP data arrives → IOCP completes → returns in 0 µs.
///      Timers use uv_backend_timeout() as the deadline so they are exact.
///      bun_wakeup() calls uv_async_send() which posts a synthetic completion
///      to the same IOCP → also wakes immediately.
///   2. Re-enqueue dequeued packets so libuv can process them later.
///   3. Fire user callback → host calls SDL_PushEvent (or equivalent).
///   4. Wait on Futex ACK until bun_run_pending_jobs() has run, preventing
///      the background thread from re-stealing packets before libuv sees them.
fn watcherThread(runtime: *BunRuntime) void {
    if (comptime Environment.isPosix) {
        const loop = runtime.vm.event_loop_handle orelse return;
        const fd: i32 = loop.fd;
        while (!runtime.event_watcher_stop.load(.acquire)) {
            // Compute wait timeout from the JS timer heap + immediate tasks.
            const hint = TickContext.getWaitHintMs(runtime);
            const poll_timeout: i32 = if (hint == 0)
                0 // Work ready now; poll returns immediately.
            else if (hint < 0)
                std.math.maxInt(i32) // No timers; block until I/O or wakeup.
            else
                @intCast(@min(hint, std.math.maxInt(i32)));

            var pfd = [1]std.posix.pollfd{.{
                .fd = fd,
                .events = std.posix.POLL.IN,
                .revents = 0,
            }};
            const n = std.posix.poll(&pfd, poll_timeout) catch 0;

            if (runtime.event_watcher_stop.load(.acquire)) break;

            // Notify when: poll returned ready events (n > 0), OR
            // the timeout expired and there was a non-negative hint (timer due).
            const should_notify = (n > 0) or (n == 0 and hint >= 0);
            if (!should_notify) continue;

            if (runtime.event_callback_fn) |cb| {
                cb(runtime.event_callback_userdata);
            }

            // Mark that we are waiting for ACK, then block until
            // bun_run_pending_jobs() signals us. This prevents redundant
            // callbacks while the host is still draining work.
            runtime.event_watcher_needs_ack.store(true, .release);
            runtime.event_watcher_ack.store(0, .release);
            while (runtime.event_watcher_ack.load(.acquire) == 0) {
                Futex.wait(&runtime.event_watcher_ack, 0);
            }
        }
    } else {
        // Windows: true IOCP blocking — zero CPU, zero I/O latency.
        const uv = bun.windows.libuv;
        const w = std.os.windows;
        const loop = runtime.vm.event_loop_handle orelse return;

        while (!runtime.event_watcher_stop.load(.acquire)) {
            // uv_backend_timeout: 0=work ready now, N>0=ms to next timer, -1=no timers.
            // Use the exact libuv deadline so timeout-based wakeups only happen
            // when work is actually due, instead of every fixed polling interval.
            const timeout_raw = uv.uv_backend_timeout(loop);
            const timeout_ms: w.DWORD = if (timeout_raw == 0)
                0 // Work is already ready; return instantly from IOCP call.
            else if (timeout_raw < 0)
                w.INFINITE // No timers pending; wait for real I/O or uv_async wakeup.
            else
                @intCast(timeout_raw);

            // Block here until real I/O completes (IOCP), a timer deadline
            // arrives (timeout_ms expiry), or bun_wakeup() posts a synthetic
            // completion (via uv_async_send → PostQueuedCompletionStatus).
            var entries: [64]w.OVERLAPPED_ENTRY = undefined;
            var n: w.ULONG = 0;
            const rc = w.kernel32.GetQueuedCompletionStatusEx(
                loop.iocp,
                &entries,
                64,
                &n,
                timeout_ms,
                0, // fAlertable = FALSE
            );

            if (runtime.event_watcher_stop.load(.acquire)) {
                // Re-enqueue any packets we just pulled so libuv can still
                // see them during clean-up or the next bun_run_pending_jobs.
                for (entries[0..n]) |entry| {
                    _ = w.kernel32.PostQueuedCompletionStatus(
                        loop.iocp,
                        entry.dwNumberOfBytesTransferred,
                        entry.lpCompletionKey,
                        entry.lpOverlapped,
                    );
                }
                break;
            }

            const should_notify = if (rc != 0)
                true
            else switch (w.kernel32.GetLastError()) {
                .TIMEOUT, .WAIT_TIMEOUT => timeout_raw >= 0,
                else => false,
            };

            if (!should_notify) {
                continue;
            }

            // Re-enqueue every packet we dequeued so libuv processes them
            // normally when bun_run_pending_jobs() calls uv_run(NOWAIT).
            for (entries[0..n]) |entry| {
                _ = w.kernel32.PostQueuedCompletionStatus(
                    loop.iocp,
                    entry.dwNumberOfBytesTransferred,
                    entry.lpCompletionKey,
                    entry.lpOverlapped,
                );
            }

            // Fire user callback — expected to do SDL_PushEvent or similar.
            if (runtime.event_callback_fn) |cb| {
                cb(runtime.event_callback_userdata);
            }

            // Mark that we are waiting for ACK, then block until
            // bun_run_pending_jobs() signals us.
            runtime.event_watcher_needs_ack.store(true, .release);
            runtime.event_watcher_ack.store(0, .release);
            while (runtime.event_watcher_ack.load(.acquire) == 0) {
                Futex.wait(&runtime.event_watcher_ack, 0);
            }
        }
    }
}

/// Stop and join the event watcher thread if one is running.
fn stopEventWatcher(runtime: *BunRuntime) void {
    if (runtime.event_watcher_thread == null) return;

    runtime.event_watcher_stop.store(true, .release);

    if (runtime.vm.event_loop_handle) |loop| loop.wakeup();

    // Wake the watcher thread's secondary blocking point (Futex wait
    // after firing the callback) so it can observe the stop flag.
    runtime.event_watcher_ack.store(1, .release);
    Futex.wake(&runtime.event_watcher_ack, 1);

    runtime.event_watcher_thread.?.join();
    runtime.event_watcher_thread = null;
    runtime.event_watcher_stop.store(false, .release);
}

pub export fn bun_set_event_callback(
    rt: ?*BunRuntime,
    cb: ?*const fn (?*anyopaque) callconv(.c) void,
    userdata: ?*anyopaque,
) callconv(.c) void {
    const runtime = rt orelse return;

    // Stop any existing watcher before updating the callback.
    stopEventWatcher(runtime);

    runtime.event_callback_fn = cb;
    runtime.event_callback_userdata = userdata;

    if (cb == null) return;

    runtime.event_watcher_stop.store(false, .release);
    runtime.event_watcher_ack.store(0, .release);
    runtime.event_watcher_thread = std.Thread.spawn(
        .{ .allocator = bun.default_allocator },
        watcherThread,
        .{runtime},
    ) catch return;
}

// ---------------------------------------------------------------------------
// Value Creation
// ---------------------------------------------------------------------------

/// Must stay in sync with BunTypedArrayKind in bun_embed.h and BunEmbed.cpp.
const BunTypedArrayKind = enum(u32) {
    int8 = 0,
    uint8 = 1,
    uint8c = 2,
    int16 = 3,
    uint16 = 4,
    int32 = 5,
    uint32 = 6,
    float32 = 7,
    float64 = 8,
    bigint64 = 9,
    biguint64 = 10,
};

pub export fn bun_bool(value: c_int) callconv(.c) BunValue {
    return toBunValue(JSValue.jsBoolean(value != 0));
}

pub export fn bun_number(value: f64) callconv(.c) BunValue {
    return toBunValue(JSValue.jsDoubleNumber(value));
}

pub export fn bun_int32(value: i32) callconv(.c) BunValue {
    return toBunValue(JSValue.jsNumberFromInt32(value));
}

pub export fn bun_string(ctx: ?*BunContext, utf8: ?[*]const u8, len: usize) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const ptr = utf8 orelse "";
    return toBunValue(BunString__createUTF8ForJS(global, ptr, len));
}

pub export fn bun_error(ctx: ?*BunContext, utf8: ?[*]const u8, len: usize) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    if (utf8 == null and len > 0) return toBunValue(.js_undefined);

    const ptr = utf8 orelse "";
    const result = global.createErrorInstance("{s}", .{ptr[0..len]});
    if (result == .zero) {
        _ = global.clearExceptionExceptTermination();
        return toBunValue(.js_undefined);
    }

    return toBunValue(result);
}

pub export fn bun_object(ctx: ?*BunContext) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    return toBunValue(JSValue.createEmptyObject(global, 0));
}

pub export fn bun_array(ctx: ?*BunContext, len: usize) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const value = JSValue.createEmptyArray(global, len) catch return toBunValue(.js_undefined);
    return toBunValue(value);
}

pub export fn bun_global(ctx: ?*BunContext) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    return toBunValue(global.toJSValue());
}

fn hostFnTrampoline(global: *JSGlobalObject, callframe: *jsc.CallFrame) callconv(jsc.conv) JSValue {
    const callee = callframe.callee();
    // Look up in the runtime's per-instance registry via the VM pointer.
    // We find the runtime by casting the VM's globalObject back: the VM is
    // embedded in BunRuntime so we stored a pointer in HostFnData itself.
    // All we need is the registry entry from the callee JSValue.
    //
    // To locate the right runtime we stored a back-pointer in HostFnData.
    // We first peek into the global's VM to find our runtime.  Since
    // BunRuntime owns the VirtualMachine we can walk it via global.bunVM().
    const vm = global.bunVM();

    // Find the BunRuntime that owns this VM.  We stored the runtime pointer
    // in each HostFnData so we just need to look up by callee value.
    // The trampoline is shared across all bun_function() calls; the callee
    // JSValue uniquely identifies which HostFnData to use.
    //
    // We need the runtime pointer to access host_fn_registry.  We stored it
    // in HostFnData.runtime so we can recover it once we have the entry — but
    // we need the registry first.  Bootstrap: keep a small thread-local cache
    // mapping vm → *BunRuntime.
    const runtime = vmToRuntime(vm) orelse return .js_undefined;
    const cb_data = runtime.host_fn_registry.get(callee) orelse return .js_undefined;
    const args = callframe.arguments();

    const result = cb_data.native_fn(
        @ptrCast(global),
        @intCast(args.len),
        if (args.len == 0) null else @as([*]const BunValue, @ptrCast(args.ptr)),
        cb_data.userdata,
    );
    if (global.hasException()) {
        return .zero;
    }
    return toJSValue(result);
}

/// GC finalizer called when a bun_function() JS function is collected.
/// Removes HostFnData from the registry and frees the allocation.
fn hostFnFinalizer(userdata: ?*anyopaque) callconv(.c) void {
    const cb_data: *HostFnData = @ptrCast(@alignCast(userdata orelse return));
    const runtime = cb_data.runtime;
    _ = runtime.host_fn_registry.remove(cb_data.js_fn);
    bun.default_allocator.destroy(cb_data);
}

pub export fn bun_function(
    ctx: ?*BunContext,
    name_ptr: ?[*]const u8,
    name_len: usize,
    native_fn: ?BunHostFn,
    userdata: ?*anyopaque,
    arg_count: c_int,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const name = if (name_ptr) |p| p[0..name_len] else return toBunValue(.js_undefined);
    const fn_ptr = native_fn orelse return toBunValue(.js_undefined);
    const runtime = vmToRuntime(global.bunVM()) orelse return toBunValue(.js_undefined);

    const js_fn = JSFunction__createFromZig(
        global,
        bun.String.init(name),
        hostFnTrampoline,
        if (arg_count >= 0) @intCast(arg_count) else 0,
        0,
        0,
        null,
    );
    if (js_fn == .zero) return toBunValue(.js_undefined);

    const cb_data = bun.default_allocator.create(HostFnData) catch return toBunValue(.js_undefined);
    cb_data.* = .{
        .native_fn = fn_ptr,
        .userdata = userdata,
        .runtime = runtime,
        .js_fn = js_fn,
    };

    runtime.host_fn_registry.put(bun.default_allocator, js_fn, cb_data) catch {
        bun.default_allocator.destroy(cb_data);
        return toBunValue(.js_undefined);
    };

    // Attach a GC finalizer so HostFnData is freed and the registry entry is
    // removed when the JS function is garbage-collected.
    if (!BunEmbed__defineFinalizer(global, js_fn, hostFnFinalizer, cb_data)) {
        // Finalizer failed — still usable, but will leak on GC (only freed at bun_destroy).
        // This is acceptable degraded behaviour rather than failing the whole call.
    }

    return toBunValue(js_fn);
}

pub export fn bun_throw(ctx: ?*BunContext, err: BunValue) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return 0;

    if (global.hasException()) return 0;

    if (err == 0) {
        _ = global.throwTypeError("bun_throw() does not accept BUN_EXCEPTION", .{}) catch {};
        return 0;
    }

    _ = global.throwValue(toJSValue(err)) catch {};
    return 0;
}

pub export fn bun_array_buffer(
    ctx: ?*BunContext,
    data: ?*anyopaque,
    len: usize,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const result = BunEmbed__createArrayBuffer(global, data, len, finalizer, userdata);
    return if (result == .zero) toBunValue(.js_undefined) else toBunValue(result);
}

pub export fn bun_typed_array(
    ctx: ?*BunContext,
    kind: u32,
    data: ?*anyopaque,
    element_count: usize,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const result = BunEmbed__createTypedArray(global, kind, data, element_count, finalizer, userdata);
    return if (result == .zero) toBunValue(.js_undefined) else toBunValue(result);
}

pub export fn bun_get_array_buffer(
    ctx: ?*BunContext,
    value: BunValue,
    out: ?*BunArrayBufferInfo,
) callconv(.c) c_int {
    const info = out orelse return 0;
    info.* = .{
        .data = null,
        .byte_length = 0,
    };

    const global = toGlobal(ctx) orelse return 0;
    return if (BunEmbed__getArrayBuffer(global, toJSValue(value), info)) 1 else 0;
}

pub export fn bun_get_typed_array(
    ctx: ?*BunContext,
    value: BunValue,
    out: ?*BunTypedArrayInfo,
) callconv(.c) c_int {
    const info = out orelse return 0;
    info.* = .{
        .data = null,
        .byte_offset = 0,
        .byte_length = 0,
        .element_count = 0,
        .kind = 0,
    };

    const global = toGlobal(ctx) orelse return 0;
    return if (BunEmbed__getTypedArray(global, toJSValue(value), info)) 1 else 0;
}

pub export fn bun_class_register(
    ctx: ?*BunContext,
    descriptor: ?*const BunClassDescriptor,
    parent: ?*BunClass,
) callconv(.c) ?*BunClass {
    const global = toGlobal(ctx) orelse return null;
    const class_descriptor = descriptor orelse return null;
    const runtime = vmToRuntime(global.bunVM()) orelse return null;

    const class_handle = BunEmbed__registerClass(global, class_descriptor, parent) orelse return null;
    runtime.class_registry.append(bun.default_allocator, class_handle) catch {
        BunEmbed__destroyClass(class_handle);
        return null;
    };
    return class_handle;
}

pub export fn bun_class_new(
    ctx: ?*BunContext,
    class_handle: ?*BunClass,
    native_ptr: ?*anyopaque,
    finalizer: ?BunClassFinalizerFn,
    userdata: ?*anyopaque,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const klass = class_handle orelse return toBunValue(.js_undefined);
    const result = BunEmbed__createClassInstance(global, klass, native_ptr, finalizer, userdata);
    return if (result == .zero) toBunValue(.js_undefined) else toBunValue(result);
}

pub export fn bun_class_unwrap(
    ctx: ?*BunContext,
    value: BunValue,
    class_handle: ?*BunClass,
) callconv(.c) ?*anyopaque {
    const global = toGlobal(ctx) orelse return null;
    return BunEmbed__unwrapClassInstance(global, toJSValue(value), class_handle);
}

pub export fn bun_is_class_instance(ctx: ?*BunContext, value: BunValue) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    return if (BunEmbed__isClassInstance(global, toJSValue(value))) 1 else 0;
}

pub export fn bun_instanceof_class(
    ctx: ?*BunContext,
    value: BunValue,
    class_handle: ?*BunClass,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const klass = class_handle orelse return 0;
    return if (BunEmbed__instanceofClass(global, toJSValue(value), klass)) 1 else 0;
}

pub export fn bun_class_dispose(ctx: ?*BunContext, value: BunValue) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    return if (BunEmbed__disposeClassInstance(global, toJSValue(value))) 1 else 0;
}

pub export fn bun_class_prototype(ctx: ?*BunContext, class_handle: ?*BunClass) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const klass = class_handle orelse return toBunValue(.js_undefined);
    const result = BunEmbed__classPrototype(global, klass);
    return if (result == .zero) toBunValue(.js_undefined) else toBunValue(result);
}

pub export fn bun_class_constructor(ctx: ?*BunContext, class_handle: ?*BunClass) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const klass = class_handle orelse return toBunValue(.js_undefined);
    const result = BunEmbed__classConstructor(global, klass);
    return if (result == .zero) toBunValue(.js_undefined) else toBunValue(result);
}

// ---------------------------------------------------------------------------
// Value Introspection & Conversion
// ---------------------------------------------------------------------------

pub export fn bun_is_undefined(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isUndefined()) 1 else 0;
}

pub export fn bun_is_null(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isNull()) 1 else 0;
}

pub export fn bun_is_bool(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isBoolean()) 1 else 0;
}

pub export fn bun_is_number(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isNumber()) 1 else 0;
}

pub export fn bun_is_string(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isString()) 1 else 0;
}

pub export fn bun_is_object(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isObject()) 1 else 0;
}

pub export fn bun_is_array(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isArray()) 1 else 0;
}

pub export fn bun_is_callable(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isCallable()) 1 else 0;
}

pub export fn bun_to_bool(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).toBoolean()) 1 else 0;
}

pub export fn bun_to_number(ctx: ?*BunContext, value: BunValue) callconv(.c) f64 {
    const global = toGlobal(ctx) orelse return std.math.nan(f64);
    return toJSValue(value).toNumber(global) catch std.math.nan(f64);
}

pub export fn bun_to_int32(value: BunValue) callconv(.c) i32 {
    return toJSValue(value).toInt32();
}

pub export fn bun_to_utf8(ctx: ?*BunContext, value: BunValue, out_len: ?*usize) callconv(.c) ?[*:0]u8 {
    const global = toGlobal(ctx) orelse return null;
    const slice = toJSValue(value).toSlice(global, bun.default_allocator) catch return null;
    defer slice.deinit();

    const out_ptr = std.c.malloc(slice.len + 1) orelse return null;
    const out_buf: [*]u8 = @ptrCast(out_ptr);
    if (slice.len > 0) {
        @memcpy(out_buf[0..slice.len], slice.ptr[0..slice.len]);
    }
    out_buf[slice.len] = 0;

    if (out_len) |len_ptr| {
        len_ptr.* = slice.len;
    }

    return @ptrCast(out_buf);
}

pub export fn bun_array_length(ctx: ?*BunContext, value: BunValue) callconv(.c) i64 {
    const global = toGlobal(ctx) orelse return -1;
    const js_val = toJSValue(value);
    const len = js_val.getLengthIfPropertyExistsInternal(global) catch return -1;
    if (len == std.math.floatMax(f64)) {
        return -1;
    }
    return @intFromFloat(std.math.clamp(len, 0, @as(f64, @floatFromInt(@as(i64, std.math.maxInt(i52))))));
}

fn clearEmbedLastError(global: *JSGlobalObject) ?*BunRuntime {
    const runtime = vmToRuntime(global.bunVM());
    if (runtime) |rt| rt.freeLastError();
    return runtime;
}

fn failEmbedWithMessage(runtime: ?*BunRuntime, message: []const u8) c_int {
    if (runtime) |rt| rt.setLastErrorBytes(message);
    return 0;
}

fn failEmbedWithException(global: *JSGlobalObject, runtime: ?*BunRuntime, exception: anytype) c_int {
    if (runtime) |rt| {
        rt.captureException(global, global.takeException(exception));
    } else {
        _ = global.takeException(exception);
    }
    return 0;
}

fn failEmbedWithThrownValue(global: *JSGlobalObject, runtime: ?*BunRuntime, thrown_value: JSValue) c_int {
    if (runtime) |rt| {
        rt.captureException(global, thrown_value);
    } else {
        global.clearException();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Object & Property Operations
// ---------------------------------------------------------------------------

pub export fn bun_set(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
    value: BunValue,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const key = if (key_ptr) |p| p[0..key_len] else return 0;
    const obj = toJSValue(object);
    if (!obj.isObject()) return 0;

    obj.put(global, key, toJSValue(value));
    return 1;
}

pub export fn bun_get(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const key = if (key_ptr) |p| p[0..key_len] else return toBunValue(.js_undefined);
    const obj = toJSValue(object);
    if (!obj.isObject()) return toBunValue(.js_undefined);

    const value = obj.getPropertyValue(global, key) catch return toBunValue(.js_undefined);
    return if (value) |v| toBunValue(v) else toBunValue(.js_undefined);
}

pub export fn bun_set_index(
    ctx: ?*BunContext,
    object: BunValue,
    index: u32,
    value: BunValue,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const obj = toJSValue(object);
    if (!obj.isObject()) return 0;

    obj.putIndex(global, index, toJSValue(value)) catch return 0;
    return 1;
}

pub export fn bun_get_index(ctx: ?*BunContext, object: BunValue, index: u32) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const obj = toJSValue(object);
    if (!obj.isObject()) return toBunValue(.js_undefined);

    const value = obj.getIndex(global, index) catch return toBunValue(.js_undefined);
    return toBunValue(value);
}

pub export fn bun_array_get_range(
    ctx: ?*BunContext,
    array: BunValue,
    start: u32,
    count: u32,
    out_values: ?[*]BunValue,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const runtime = clearEmbedLastError(global);
    const js_array = toJSValue(array);

    if (!js_array.isArray()) {
        return failEmbedWithMessage(runtime, "value is not a JavaScript Array");
    }

    _ = std.math.add(u32, start, count) catch {
        return failEmbedWithMessage(runtime, "array range overflows uint32_t");
    };

    if (count == 0) return 1;

    const out = out_values orelse return failEmbedWithMessage(runtime, "out_values is null");

    if (!BunEmbed__arrayGetRange(global, js_array, start, count, out)) {
        if (global.tryTakeException()) |exc| {
            return failEmbedWithThrownValue(global, runtime, exc);
        }
        return failEmbedWithMessage(runtime, "array range read failed");
    }

    return 1;
}

pub export fn bun_array_set_range(
    ctx: ?*BunContext,
    array: BunValue,
    start: u32,
    count: u32,
    values: ?[*]const BunValue,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const runtime = clearEmbedLastError(global);
    const js_array = toJSValue(array);

    if (!js_array.isArray()) {
        return failEmbedWithMessage(runtime, "value is not a JavaScript Array");
    }

    const end = std.math.add(u32, start, count) catch {
        return failEmbedWithMessage(runtime, "array range overflows uint32_t");
    };

    if (count == 0) return 1;

    const length = js_array.getLengthIfPropertyExistsInternal(global) catch |err| {
        return failEmbedWithException(global, runtime, err);
    };
    if (length == std.math.floatMax(f64)) {
        return failEmbedWithMessage(runtime, "array length is unavailable");
    }
    if (length < @as(f64, @floatFromInt(end))) {
        return failEmbedWithMessage(runtime, "array range exceeds current length");
    }

    const input = values orelse return failEmbedWithMessage(runtime, "values is null");

    if (!BunEmbed__arraySetRange(global, js_array, start, count, input)) {
        if (global.tryTakeException()) |exc| {
            return failEmbedWithThrownValue(global, runtime, exc);
        }
        return failEmbedWithMessage(runtime, "array element write was rejected");
    }

    return 1;
}

pub export fn bun_define_getter(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
    getter: ?BunGetterFn,
    userdata: ?*anyopaque,
    dont_enum: c_int,
    dont_delete: c_int,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const key = key_ptr orelse return 0;
    const callback = getter orelse return 0;

    var flags: u32 = 0;
    if (dont_enum != 0) flags |= BUN_ACCESSOR_DONT_ENUM;
    if (dont_delete != 0) flags |= BUN_ACCESSOR_DONT_DELETE;

    return if (BunEmbed__defineCustomAccessor(
        global,
        toJSValue(object),
        key,
        key_len,
        callback,
        userdata,
        null,
        null,
        BUN_ACCESSOR_UPDATE_GETTER,
        flags,
    )) 1 else 0;
}

pub export fn bun_define_setter(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
    setter: ?BunSetterFn,
    userdata: ?*anyopaque,
    dont_enum: c_int,
    dont_delete: c_int,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const key = key_ptr orelse return 0;
    const callback = setter orelse return 0;

    var flags: u32 = 0;
    if (dont_enum != 0) flags |= BUN_ACCESSOR_DONT_ENUM;
    if (dont_delete != 0) flags |= BUN_ACCESSOR_DONT_DELETE;

    return if (BunEmbed__defineCustomAccessor(
        global,
        toJSValue(object),
        key,
        key_len,
        null,
        null,
        callback,
        userdata,
        BUN_ACCESSOR_UPDATE_SETTER,
        flags,
    )) 1 else 0;
}

pub export fn bun_define_accessor(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
    getter: ?BunGetterFn,
    setter: ?BunSetterFn,
    userdata: ?*anyopaque,
    read_only: c_int,
    dont_enum: c_int,
    dont_delete: c_int,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const key = key_ptr orelse return 0;
    if (getter == null and setter == null) return 0;

    var flags: u32 = 0;
    if (read_only != 0) flags |= BUN_ACCESSOR_READ_ONLY;
    if (dont_enum != 0) flags |= BUN_ACCESSOR_DONT_ENUM;
    if (dont_delete != 0) flags |= BUN_ACCESSOR_DONT_DELETE;

    var update_mask: u8 = 0;
    if (getter != null) update_mask |= BUN_ACCESSOR_UPDATE_GETTER;
    if (setter != null) update_mask |= BUN_ACCESSOR_UPDATE_SETTER;

    return if (BunEmbed__defineCustomAccessor(
        global,
        toJSValue(object),
        key,
        key_len,
        getter,
        userdata,
        setter,
        userdata,
        update_mask,
        flags,
    )) 1 else 0;
}

pub export fn bun_define_finalizer(
    ctx: ?*BunContext,
    object: BunValue,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const callback = finalizer orelse return 0;
    const obj = toJSValue(object);
    if (!obj.isObject()) return 0;
    const runtime = vmToRuntime(global.bunVM()) orelse return 0;

    // Use the opaque_map to guard against double-attachment.
    const result = runtime.opaque_map.getOrPut(bun.default_allocator, obj) catch return 0;
    if (result.found_existing and result.value_ptr.finalizer_attached) return 0;
    if (!result.found_existing) result.value_ptr.* = .{};
    if (!BunEmbed__defineFinalizer(global, obj, callback, userdata)) return 0;

    result.value_ptr.finalizer_attached = true;
    return 1;
}

pub export fn bun_set_prototype(ctx: ?*BunContext, object: BunValue, proto: BunValue) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const obj = toJSValue(object);
    const prototype = toJSValue(proto);

    if (!obj.isObject()) return 0;
    if (!prototype.isObject() and !prototype.isNull()) return 0;

    obj.setPrototypeDirect(prototype, global) catch return 0;
    return 1;
}

pub export fn bun_set_opaque(ctx: ?*BunContext, object: BunValue, opaque_ptr: ?*anyopaque) callconv(.c) void {
    const global = toGlobal(ctx) orelse return;
    const obj = toJSValue(object);
    if (!obj.isObject()) return;
    const runtime = vmToRuntime(global.bunVM()) orelse return;

    const result = runtime.opaque_map.getOrPut(bun.default_allocator, obj) catch return;
    if (!result.found_existing) result.value_ptr.* = .{};
    result.value_ptr.opaque_ptr = opaque_ptr;
}

pub export fn bun_get_opaque(ctx: ?*BunContext, object: BunValue) callconv(.c) ?*anyopaque {
    const global = toGlobal(ctx) orelse return null;
    const obj = toJSValue(object);
    if (!obj.isObject()) return null;
    const runtime = vmToRuntime(global.bunVM()) orelse return null;

    const entry = runtime.opaque_map.get(obj) orelse return null;
    return entry.opaque_ptr;
}

// ---------------------------------------------------------------------------
// Function Call & GC Lifetime
// ---------------------------------------------------------------------------

pub export fn bun_call(
    ctx: ?*BunContext,
    fn_value: BunValue,
    this_value: BunValue,
    argc: c_int,
    argv: ?[*]const BunValue,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return 0;
    const runtime = vmToRuntime(global.bunVM());
    // Clear any stale error so bun_last_error(ctx, ...) is NULL after a successful call.
    if (runtime) |rt| rt.freeLastError();

    const function = toJSValue(fn_value);
    if (!function.isCallable()) return 0;

    const argc_u: usize = if (argc > 0) @intCast(argc) else 0;
    const args: []const JSValue = if (argc_u == 0)
        &.{}
    else if (argv) |p|
        @as([*]const JSValue, @ptrCast(p))[0..argc_u]
    else
        return 0;

    const result = function.call(global, toJSValue(this_value), args) catch |err| {
        // Capture the exception message into last_error_buf so the caller
        // can retrieve it with bun_last_error(ctx, ...).
        if (runtime) |rt| {
            rt.captureException(global, global.takeException(err));
        } else {
            global.clearException();
        }
        return 0; // BUN_EXCEPTION sentinel
    };

    if (global.tryTakeException()) |exc| {
        if (runtime) |rt| {
            rt.captureException(global, exc);
        } else {
            global.clearException();
        }
        return 0;
    }

    if (result == .zero)
        return 0;

    return toBunValue(result);
}

/// Return the latest error string stored by bun_call() or bun_eval*().
pub export fn bun_last_error(ctx: ?*BunContext, out_len: ?*usize) callconv(.c) ?[*:0]const u8 {
    if (out_len) |len_ptr| len_ptr.* = 0;

    const global = toGlobal(ctx) orelse return null;
    const runtime = vmToRuntime(global.bunVM()) orelse return null;

    if (runtime.last_error_buf) |buf| {
        if (out_len) |len_ptr| len_ptr.* = std.mem.len(buf);
        return buf;
    }

    return null;
}

pub export fn bun_call_async(
    ctx: ?*BunContext,
    fn_value: BunValue,
    this_value: BunValue,
    argc: c_int,
    argv: ?[*]const BunValue,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const runtime = vmToRuntime(global.bunVM()) orelse return 0;
    const argc_u: usize = if (argc > 0) @intCast(argc) else 0;

    var arg_copy: []BunValue = &.{};
    if (argc_u > 0) {
        const src = argv orelse return 0;
        arg_copy = bun.default_allocator.alloc(BunValue, argc_u) catch return 0;
        @memcpy(arg_copy, src[0..argc_u]);
    }

    runtime.pending_calls_mutex.lock();
    defer runtime.pending_calls_mutex.unlock();

    runtime.pending_calls.append(bun.default_allocator, .{
        .fn_value = fn_value,
        .this_value = this_value,
        .argv = arg_copy,
    }) catch {
        if (arg_copy.len > 0) bun.default_allocator.free(arg_copy);
        return 0;
    };

    if (runtime.vm.event_loop_handle) |loop| {
        loop.wakeup();
    }

    return 1;
}

pub export fn bun_protect(_: ?*BunContext, value: BunValue) callconv(.c) void {
    toJSValue(value).protect();
}

pub export fn bun_unprotect(_: ?*BunContext, value: BunValue) callconv(.c) void {
    toJSValue(value).unprotect();
}
comptime {
    _ = &bun_initialize;
    _ = &bun_destroy;
    _ = &bun_context;
    _ = &bun_eval_string;
    _ = &bun_eval_file;
    _ = &bun_run_pending_jobs;
    _ = &bun_get_event_fd;
    _ = &bun_get_wait_hint;
    _ = &bun_wakeup;
    _ = &bun_set_event_callback;
    _ = &bun_bool;
    _ = &bun_number;
    _ = &bun_int32;
    _ = &bun_string;
    _ = &bun_object;
    _ = &bun_array;
    _ = &bun_global;
    _ = &bun_function;
    _ = &bun_error;
    _ = &bun_throw;
    _ = &bun_array_buffer;
    _ = &bun_typed_array;
    _ = &bun_get_array_buffer;
    _ = &bun_get_typed_array;
    _ = &bun_class_register;
    _ = &bun_class_new;
    _ = &bun_class_unwrap;
    _ = &bun_is_class_instance;
    _ = &bun_instanceof_class;
    _ = &bun_class_dispose;
    _ = &bun_class_prototype;
    _ = &bun_class_constructor;
    _ = &bun_is_undefined;
    _ = &bun_is_null;
    _ = &bun_is_bool;
    _ = &bun_is_number;
    _ = &bun_is_string;
    _ = &bun_is_object;
    _ = &bun_is_array;
    _ = &bun_is_callable;
    _ = &bun_to_bool;
    _ = &bun_to_number;
    _ = &bun_to_int32;
    _ = &bun_to_utf8;
    _ = &bun_array_length;
    _ = &bun_set;
    _ = &bun_get;
    _ = &bun_set_index;
    _ = &bun_get_index;
    _ = &bun_array_get_range;
    _ = &bun_array_set_range;
    _ = &bun_define_getter;
    _ = &bun_define_setter;
    _ = &bun_define_accessor;
    _ = &bun_define_finalizer;
    _ = &bun_set_opaque;
    _ = &bun_get_opaque;
    _ = &bun_call;
    _ = &bun_last_error;
    _ = &bun_call_async;
    _ = &bun_protect;
    _ = &bun_unprotect;
}
