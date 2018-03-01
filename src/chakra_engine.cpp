/*
 * Copyright 2018, alex at staticlibs.net
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
    if (JsNoError != err_global) throw support::exception(TRACEMSG(
            "'JsGetGlobalObject' error, func name: [" + name + "]," +
            " code: [" + sl::support::to_string(err_global) + "]"));

    JsPropertyIdRef prop = JS_INVALID_REFERENCE;
    auto wname = sl::utils::widen(name);
    auto err_prop = JsGetPropertyIdFromName(wname.c_str(), std::addressof(prop));
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
    auto err_convert = JsConvertValueToString(val, std::addressof(val_str));
    if (JsNoError != err_convert) return "";

    // extract string
    int len = 0;
    auto err_size = JsGetStringLength(val_str, std::addressof(len));
    if (JsNoError != err_size) return "";
    if (0 == len) return "";
    size_t written = 0;
    const wchar_t* wptr = nullptr;
    auto err_str = JsStringToPointer(val_str, std::addressof(wptr), std::addressof(written));
    if (JsNoError != err_str) return "";
    return sl::utils::narrow(wptr, written);
}

std::string format_stack_trace(JsErrorCode err) STATICLIB_NOEXCEPT {
    auto default_msg = std::string() + "Error code: [" + sl::support::to_string(err) + "]";
    JsValueRef exc = JS_INVALID_REFERENCE;
    auto err_get = JsGetAndClearException(std::addressof(exc));
    if (JsNoError != err_get) {
        return default_msg;
    }
    JsPropertyIdRef prop = JS_INVALID_REFERENCE;
    auto wname = sl::utils::widen("stack");
    auto err_prop = JsGetPropertyIdFromName(wname.c_str(), std::addressof(prop));
    if (JsNoError != err_prop) {
        return default_msg;
    }
    JsValueRef stack_ref = JS_INVALID_REFERENCE;
    auto err_stack = JsGetProperty(exc, prop, std::addressof(stack_ref));
    if (JsNoError != err_stack) {
        return default_msg;
    }
    auto stack = jsval_to_string(stack_ref);
    // filter and format
    auto vec = sl::utils::split(stack, '\n');
    auto res = std::string();
    for (size_t i = 0; i < vec.size(); i++) {
        auto& line = vec.at(i);
        if(line.length() > 1 && !(std::string::npos != line.find("(wilton-requirejs/require.js:")) &&
                !(std::string::npos != line.find("(wilton-require.js:"))) {
            if (sl::utils::starts_with(line, "   at")) {
                res.push_back(' ');
            }
            res += line;
            res.push_back('\n');
        }
    }
    if (res.length() > 0 && '\n' == res.back()) {
        res.pop_back();
    }
    return res;
}

bool is_string_ref(JsValueRef val) {
    JsValueType vt = JsUndefined;
    auto err_type = JsGetValueType(val, std::addressof(vt));
    if (JsNoError != err_type) throw support::exception(TRACEMSG(
            "'JsGetValueType' error, code: [" + sl::support::to_string(err_type) + "]"));
    return JsString == vt;
}

std::string eval_js(const char* code, const std::string& path) {
    auto wcode = sl::utils::widen(code);
    auto wpath = sl::utils::widen(path);
    auto hasher = std::hash<std::string>();
    auto src_ctx = static_cast<JsSourceContext>(hasher(path));
    JsValueRef res = JS_INVALID_REFERENCE;
    auto err = JsRunScript(wcode.c_str(), src_ctx, wpath.c_str(), std::addressof(res));
    if (JsErrorInExceptionState == err) {
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

JsValueRef create_error(const std::string& msg) STATICLIB_NOEXCEPT {
    JsValueRef str = JS_INVALID_REFERENCE;
    auto wmsg = sl::utils::widen(msg);
    auto err_str = JsPointerToString(wmsg.c_str(), wmsg.length(), std::addressof(str));
    if (JsNoError != err_str) {
        // fallback
        auto wem = sl::utils::widen("ERROR");
        JsPointerToString(wem.c_str(), wem.length(), std::addressof(str));
    }
    JsValueRef res = JS_INVALID_REFERENCE;
    auto err_err = JsCreateError(str, std::addressof(res));
    if (JsNoError != err_err) {
        // fallback, todo: check me
        JsCreateError(JS_INVALID_REFERENCE, std::addressof(res));
    }
    return res;
}

JsValueRef CALLBACK print_func(JsValueRef /* callee */, bool /* is_construct_call */,
        JsValueRef* args, unsigned short args_count, void* /* callback_state */) STATICLIB_NOEXCEPT {
    if (args_count > 1) {
        auto val = jsval_to_string(args[1]);
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
        if (args_count < 2 || !is_string_ref(args[1])) {
            throw support::exception(TRACEMSG("Invalid arguments specified"));
        }

        // load code
        auto path = jsval_to_string(args[1]);
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
        eval_js(code, path_short);
        wilton::support::log_debug("wilton.engine.chakra.eval", "Eval complete");
    } catch (const std::exception& e) {
        auto msg = TRACEMSG(e.what() + "\nError(e) loading script, path: [" + path + "]");
        auto err = create_error(msg);
        JsSetException(err);
        return JS_INVALID_REFERENCE;
    } catch (...) {
        auto msg = TRACEMSG("Error(...) loading script, path: [" + path + "]");
        auto err = create_error(msg);
        JsSetException(err);
        return JS_INVALID_REFERENCE;
    }
    return JS_INVALID_REFERENCE;
}

JsValueRef CALLBACK wiltoncall_func(JsValueRef /* callee */, bool /* is_construct_call */,
        JsValueRef* args, unsigned short args_count, void* /* callback_state */) STATICLIB_NOEXCEPT {
    if (args_count < 3 || !is_string_ref(args[1]) || !is_string_ref(args[2])) {
        auto msg = TRACEMSG("Invalid arguments specified");
        auto err = create_error(msg);
        JsSetException(err);
        return JS_INVALID_REFERENCE;
    }
    auto name = jsval_to_string(args[1]);
    auto input = jsval_to_string(args[2]);
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
            auto wout = sl::utils::widen(out, static_cast<size_t>(out_len));
            auto err_str = JsPointerToString(wout.c_str(), wout.length(), std::addressof(res));
            if (JsNoError != err_str) {
                // fallback
                auto wem = sl::utils::widen("ERROR");
                JsPointerToString(wem.c_str(), wem.length(), std::addressof(res));
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
        if (nullptr != runtime) {
            JsDisableRuntimeExecution(runtime);
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
        auto wcb = sl::utils::widen(callback_script_json.data(), callback_script_json.size());
        auto err_arg = JsPointerToString(wcb.c_str(), wcb.length(), std::addressof(cb_arg_ref));
        if (JsNoError != err_arg) throw support::exception(TRACEMSG(
                "'JsCreateString' error, code: [" + sl::support::to_string(err_arg) + "]"));
        JsPropertyIdRef fun_prop = JS_INVALID_REFERENCE;
        auto wname = sl::utils::widen("WILTON_run");
        auto err_prop = JsGetPropertyIdFromName(wname.c_str(), std::addressof(fun_prop));
        if (JsNoError != err_prop) throw support::exception(TRACEMSG(
                "'JsCreatePropertyId' error, code: [" + sl::support::to_string(err_prop) + "]"));
        JsValueRef fun = JS_INVALID_REFERENCE;
        auto err_get = JsGetProperty(global, fun_prop, std::addressof(fun));
        if (JsNoError != err_get) throw support::exception(TRACEMSG(
                "'JsGetProperty' error, code: [" + sl::support::to_string(err_get) + "]"));
        JsValueType fun_type = JsUndefined;
        auto err_type = JsGetValueType(fun, std::addressof(fun_type));
        if (JsNoError != err_type) throw support::exception(TRACEMSG(
                "'JsGetValueType' error, code: [" + sl::support::to_string(err_type) + "]"));
        if (JsFunction != fun_type) throw support::exception(TRACEMSG(
                "Error accessing 'WILTON_run' function: not a function"));
        JsValueRef null_ref = JS_INVALID_REFERENCE;
        auto err_null = JsGetNullValue(std::addressof(null_ref));
        if (JsNoError != err_null) throw support::exception(TRACEMSG(
                "'JsGetNullValue' error, code: [" + sl::support::to_string(err_null) + "]"));
        // call
        auto args = std::array<JsValueRef, 2>();
        args[0] = null_ref;
        args[1] = cb_arg_ref;
        JsValueRef res = JS_INVALID_REFERENCE;
        auto err_call = JsCallFunction(fun, args.data(), static_cast<unsigned short>(args.size()), std::addressof(res));
        wilton::support::log_debug("wilton.engine.jsc.run",
                "Callback run complete, result: [" + sl::support::to_string_bool(JsNoError == err_call) + "]");
        if (JsNoError != err_call) {
            throw support::exception(TRACEMSG(format_stack_trace(err_call)));
        }
        if (is_string_ref(res)) {
            auto str = jsval_to_string(res);
            return support::make_string_buffer(str);
        }
        return support::make_null_buffer();
    }
};

PIMPL_FORWARD_CONSTRUCTOR(chakra_engine, (sl::io::span<const char>), (), support::exception)
PIMPL_FORWARD_METHOD(chakra_engine, support::buffer, run_callback_script, (sl::io::span<const char>), (), support::exception)

} // namespace
}

