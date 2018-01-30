/* 
 * File:   chakra_engine.cpp
 * Author: alex
 *
 * Created on January 30, 2018, 2:10 PM
 */

#include "chakra_engine.hpp"

#include <cstdio>
#include <array>
#include <functional>
#include <memory>

#include <jsrt.h>

#include "staticlib/io.hpp"
#include "staticlib/json.hpp"
#include "staticlib/pimpl/forward_macros.hpp"
#include "staticlib/support.hpp"
#include "staticlib/utils.hpp"

#include "wilton/wiltoncall.h"
#include "wilton/wilton_loader.h"

#include "wilton/support/exception.hpp"
#include "wilton/support/logging.hpp"

namespace wilton {
namespace chakra {

namespace { // anonymous

void register_c_func(const std::string& name, JsNativeFunction cb) {
    JsValueRef global = JS_INVALID_REFERENCE;
    auto err_global = JsGetGlobalObject(std::addressof(global));
    if (JsNoError != create_err) throw support::exception(TRACEMSG(
            "'JsGetGlobalObject' error, func name: [" + name + "]," +
            " code: [" + sl::support::to_string(err_global) + "]"));

    JsPropertyIdRef prop = JS_INVALID_REFERENCE;
    auto err_prop = JsCreatePropertyId(name.c_str(), name.length(), std::addressof(prop));
    if (JsNoError != err_prop) throw support::exception(TRACEMSG(
            "'JsCreatePropertyId' error, func name: [" + name + "]," +
            " code: [" + sl::support::to_string(err_prop) + "]"));

    JsValueRef func = JS_INVALID_REFERENCE;
    auto err_create = JsCreateFunction(cb, nullptr, std::addressof(func));
    if (JsNoError != err_create) throw support::exception(TRACEMSG(
            "'JsCreateFunction' error, func name: [" + name + "]," +
            " code: [" + sl::support::to_string(err_create) + "]"));

    auto err_set = JsSetProperty(global, prop, func, true);
    if (JsNoError != err_set) throw support::exception(TRACEMSG(
            "'JsSetProperty' error, func name: [" + name + "]," +
            " code: [" + sl::support::to_string(err_create) + "]"));
}

std::string jsval_to_string(JsValueRef val) STATICLIB_NOEXCEPT {
    // convert to string
    JsValueRef val_str = JS_INVALID_REFERENCE;
    auto err_convert = JsConvertValueToString(val, val_str);
    if (JsNoError != err_convert) return "";
    auto deferred = sl::support::defer([&val_str]() STATICLIB_NOEXCEPT {
        // todo: check release
        JsRelease(val_str);
    });

    // extract string
    size_t len = 0;
    auto err_size = JsCopyString(val_str, nullptr, 0, std::addressof(len));
    if (JsNoError != err_size) return "";
    if (0 == len) return "";
    auto str = std::string();
    str.resize(len);
    size_t written = 0;
    auto err_str = JsCopyString(val_str, std::addressof(str.front()), str.length(), std::addressof(written));
    if (JsNoError != err_str) return "";
    if (written < str.length()) {
        str.resize(written);
    }
    return str;
}

std::string format_stack_trace(JsErrorCode err) STATICLIB_NOEXCEPT {
    auto default_msg = std::string() + "Error code: [" + sl::support::to_string(err) + "]";
    JsValueRef exc = JS_INVALID_REFERENCE;
    auto err_get = JsGetAndClearException(std::addressof(exc));
    if (JsNoError != err_get) {
        return default_msg;
    }
    JsPropertyIdRef prop = JS_INVALID_REFERENCE;
    auto err_prop = JsGetPropertyIdFromName("stack", std::addressof(prop));
    if (JsNoError != err_prop) {
        return default_msg;
    }
    JsValueRef stack_ref = JS_INVALID_REFERENCE;
    auto err_stack = JsGetProperty(exc, prop, std::addressof(stack_ref));
    if (JsNoError != err_prop) {
        return default_msg;
    }
    return jsval_to_string(stack_ref);
}

bool is_string_ref(JsValueRef val) {
    JsValueType vt = JsUndefined;
    auto err_type = JsGetValueType(res, arguments[0]);
    if (JsNoError != err_type) throw support::exception(TRACEMSG(
            "'JsGetValueType' error, code: [" + sl::support::to_string(err_type) + "]"));
    return JsString == vt;
}

std::string eval_js(JSContextRef ctx, const char* code, const std::string& path) {
    auto wcode = sl::utils::widen(code);
    auto wpath = sl::utils::widen(path);
    JsValueRef res = JS_INVALID_REFERENCE;
    auto err = JsRunScript(wcode, JS_SOURCE_CONTEXT_NONE, wpath, std::addressof(res));
    if (JsErrorInExceptionState != err) {
        throw support::exception(TRACEMSG(format_stack_trace(err)));
    }
    if (JsNoError != err) {
        throw support::exception(TRACEMSG("'JsRunScript' error, path: [" + path + "]," +
                " err: [" + sl::support::to_string(err) + "]"));
    }
    if (JS_INVALID_REFERENCE != res) {
        JsValueType vt = JsUndefined;
        auto err_type = JsGetValueType(res, std::addressof(vt));
        if (JsNoError != err_type) throw support::exception(TRACEMSG(
                "'JsGetValueType' error, path: [" + path + "]," +
                " code: [" + sl::support::to_string(err_type) + "]"));
        if (JsString == vt) {
            return jsval_to_string(res);
        }
    }
    return "";
}

// todo: check need release
JsValueRef create_error(const std::string& msg) STATICLIB_NOEXCEPT {
    JsValueRef str = JS_INVALID_REFERENCE;
    auto err_str = JsCreateString(msg.c_str(), msg.length(), std::addressof(str));
    if (JsNoError != err_type) {
        // fallback
        auto em = std::string("ERROR");
        JsCreateString(em.c_str(), em.length(), std::addressof(str));
    }
    auto deferred = sl::support::defer([str]() STATICLIB_NOEXCEPT {
        // todo: check release
        JsRelease(str);
    });
    JsValueRef res = JS_INVALID_REFERENCE;
    auto err_err = JsCreateError(str, std::addressof(res));
    if (JsNoError != err_err) {
        // fallback, todo: check me
        JsCreateError(JS_INVALID_REFERENCE, std::addressof(res))
    }
    return res;
}

JsValueRef CALLBACK print_func(JsValueRef /* callee */, bool /* is_construct_call */,
        JsValueRef* args, unsigned short args_count, void* /* callback_state */) STATICLIB_NOEXCEPT {
    if (args_count > 0) {
        auto val = jsval_to_string(ctx, arguments[0]);
        puts(val.c_str());
    } else {
        puts("");
    }
    return JS_INVALID_REFERENCE;
}

JsValueRef CALLBACK load_func(JsValueRef /* callee */, bool /* is_construct_call */,
        JsValueRef* args, unsigned short args_count, void* /* callback_state */) STATICLIB_NOEXCEPT {
    std::string path = "";
    try {
        // check args
        if (args_count < 1 || !is_string_ref(arguments[0])) {
            throw support::exception(TRACEMSG("Invalid arguments specified"));
        }

        // load code
        auto path = jsval_to_string(ctx, arguments[0]);
        char* code = nullptr;
        int code_len = 0;
        auto err_load = wilton_load_resource(path.c_str(), static_cast<int>(path.length()),
                std::addressof(code), std::addressof(code_len));
        if (nullptr != err_load) {
            support::throw_wilton_error(err_load, TRACEMSG(err_load));
        }
        auto deferred = sl::support::defer([code] () STATICLIB_NOEXCEPT {
            wilton_free(code);
        });
        auto path_short = support::script_engine_map_detail::shorten_script_path(path);
        wilton::support::log_debug("wilton.engine.chakra.eval",
                "Evaluating source file, path: [" + path + "] ...");
        eval_js(ctx, code, path_short);
        wilton::support::log_debug("wilton.engine.chakra.eval", "Eval complete");
    } catch (const std::exception& e) {
        auto msg = TRACEMSG(e.what() + "\nError loading script, path: [" + path + "]");
        auto err = create_error(msg);
        JsSetException(err);
        return JS_INVALID_REFERENCE;
    } catch (...) {
        auto msg = TRACEMSG("Error loading script, path: [" + path + "]");
        auto err = create_error(msg);
        JsSetException(err);
        return JS_INVALID_REFERENCE;
    }
    return JS_INVALID_REFERENCE;
}

JsValueRef CALLBACK wiltoncall_func(JsValueRef /* callee */, bool /* is_construct_call */,
        JsValueRef* args, unsigned short args_count, void* /* callback_state */) STATICLIB_NOEXCEPT {
    if (args_count < 2 || !is_string_ref(arguments[0]) || !is_string_ref(ctx, arguments[1])) {
        auto msg = TRACEMSG("Invalid arguments specified");
        auto err = create_error(msg);
        JsSetException(err);
        return JS_INVALID_REFERENCE;
    }
    auto name = jsval_to_string(arguments[0]);
    auto input = jsval_to_string(arguments[1]);
    char* out = nullptr;
    int out_len = 0;
    wilton::support::log_debug("wilton.wiltoncall." + name,
            "Performing a call,  input length: [" + sl::support::to_string(input.length()) + "] ...");
    auto err = wiltoncall(name.c_str(), static_cast<int> (name.length()),
            input.c_str(), static_cast<int> (input.length()),
            std::addressof(out), std::addressof(out_len));
    wilton::support::log_debug("wilton.wiltoncall." + name,
            "Call complete, result: [" + (nullptr != err ? std::string(err) : "") + "]");
    if (nullptr == err) {
        if (nullptr != out) {
            JsValueRef res = JS_INVALID_REFERENCE;
            // todo: release
            auto err_str = JsCreateString(out, out_len, std::addressof(res));
            if (JsNoError != err_str) {
                // fallback
                auto em = std::string("ERROR");
                JsCreateString(em.c_str(), em.length(), std::addressof(res));
            }
            wilton_free(out);
            return res;
        } else {
            JsValueRef null_ref = JS_INVALID_REFERENCE;
            JsGetNullValue(std::addressof(null_ref));
            return null_ref;
        }
    } else {
        auto msg = TRACEMSG(err + "\n'wiltoncall' error for name: [" + name + "]");
        wilton_free(err);
        auto err = create_error(msg);
        JsSetException(err);
        return JS_INVALID_REFERENCE;
    }
}

} // namespace

class chakra_engine::impl : public sl::pimpl::object::impl {
    JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;

public:
    ~impl() STATICLIB_NOEXCEPT {
        JsSetCurrentContext(JS_INVALID_REFERENCE)
        if (nullptr != ctxgroup) {
            JsDisposeRuntime(runtime);
        }
    }
    
    impl(sl::io::span<const char> init_code) {
        wilton::support::log_info("wilton.engine.chakra.init", "Initializing engine instance ...");
        auto err_runtime = JsCreateRuntime(JsRuntimeAttributeNone, JsRuntimeVersion11, nullptr, std::addressof(this->runtime));
        if (JsNoError != err_runtime) throw support::exception(TRACEMSG(
                "'JsCreateRuntime' error, code: [" + sl::support::to_string(err_runtime) + "]"));
        JsContextRef ctx = JS_INVALID_REFERENCE;
        auto err_ctx = JsCreateContext(runtime, nullptr, std::addressof(ctx));
        if (JsNoError != err_runtime) throw support::exception(TRACEMSG(
                "'JsCreateContext' error, code: [" + sl::support::to_string(err_ctx) + "]"));
        auto err_set = JsSetCurrentContext(ctx);
        if (JsNoError != err_set) throw support::exception(TRACEMSG(
                "'JsSetCurrentContext' error, code: [" + sl::support::to_string(err_set) + "]"));
        register_c_func("print", print_func);
        register_c_func("WILTON_load", load_func);
        register_c_func("WILTON_wiltoncall", wiltoncall_func);
        eval_js(init_code.data(), "wilton-require.js");
        wilton::support::log_info("wilton.engine.chakra.init", "Engine initialization complete");
    }

    support::buffer run_callback_script(chakra_engine&, sl::io::span<const char> callback_script_json) {
        wilton::support::log_debug("wilton.engine.chakra.run",
                "Running callback script: [" + std::string(callback_script_json.data(), callback_script_json.size()) + "] ...");
        // extract wilton_run
        JsValueRef global = JS_INVALID_REFERENCE;
        auto err_global = JsGetGlobalObject(&global);
        if (JsNoError != err_global) throw support::exception(TRACEMSG(
                "'JsGetGlobalObject' error, code: [" + sl::support::to_string(err_global) + "]"));
        JsValueRef cb_arg_ref = JS_INVALID_REFERENCE;
        auto err_arg = JsCreateString(callback_script_json.data(), callback_script_json.size(), std::addressof(cb_arg_ref));
        if (JsNoError != err_arg) throw support::exception(tracemsg(
                "'JsCreateString' error, code: [" + sl::support::to_string(err_arg) + "]"));
        JsPropertyIdRef fun_prop = JS_INVALID_REFERENCE;
        auto fun_name = std::string("WILTON_run");
        auto err_prop = JsCreatePropertyId(fun_name, fun_name.length(), std::addressof(fun_prop));
        if (JsNoError != err_prop) throw support::exception(tracemsg(
                "'JsCreatePropertyId' error, code: [" + sl::support::to_string(err_prop) + "]"));
        JsValueRef fun = JS_INVALID_REFERENCE;
        auto err_get = JsGetProperty(global, fun_prop, std::addressof(fun));
        if (JsNoError != err_get) throw support::exception(tracemsg(
                "'JsGetProperty' error, code: [" + sl::support::to_string(err_get) + "]"));
        JsValueRef fun_type = JS_INVALID_REFERENCE;
        auto err_type = JsGetValueType(fun, std::addressof(fun_type));
        if (JsNoError != err_type) throw support::exception(tracemsg(
                "'JsGetValueType' error, code: [" + sl::support::to_string(err_type) + "]"));
        if (JsString != fun_type) throw support::exception(tracemsg(
                "Error accessing 'WILTON_run' function: not a function"));
        JsValueRef null_ref = JS_INVALID_REFERENCE;
        auto err_null = JsGetNullValue(std::addressof(null_ref));
        if (JsNoError != err_null) throw support::exception(tracemsg(
                "'JsGetNullValue' error, code: [" + sl::support::to_string(err_null) + "]"));
        // call
        auto args = std::array<JsValueRef, 2>();
        args[0] = null_ref;
        args[1] = cb_arg_ref;
        JsValueRef res = JS_INVALID_REFERENCE;
        auto err_call = JsCallFunction(fun, args.data(), args.size(), std::addressof(res));
        if (JsNoError != err_call) {
            throw support::exception(TRACEMSG(format_stack_trace(err_call)));
        }
        if (is_string_ref(res)) {
            auto str = jsval_to_string(res);
            return support::make_string_buffer(str);
        }
        return support::make_empty_buffer();
    }
};

PIMPL_FORWARD_CONSTRUCTOR(jsc_engine, (sl::io::span<const char>), (), support::exception)
PIMPL_FORWARD_METHOD(jsc_engine, support::buffer, run_callback_script, (sl::io::span<const char>), (), support::exception)

} // namespace
}

