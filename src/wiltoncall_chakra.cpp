/* 
 * File:   wiltoncall_chakra.cpp
 * Author: alex
 *
 * Created on January 30, 2018, 2:10 PM
 */
#include <memory>
#include <string>

#include "staticlib/config.hpp"
#include "staticlib/io.hpp"

#include "wilton/wilton.h"

#include "wilton/support/buffer.hpp"
#include "wilton/support/exception.hpp"
#include "wilton/support/registrar.hpp"
#include "wilton/support/script_engine_map.hpp"

#include "chakra_engine.hpp"

namespace wilton {
namespace chakra {

std::shared_ptr<support::script_engine_map<chakra_engine>> shared_tlmap() {
    static auto tlmap = std::make_shared<support::script_engine_map<chakra_engine>>();
    return tlmap;
}

support::buffer runscript(sl::io::span<const char> data) {
    auto tlmap = shared_tlmap();
    return tlmap->run_script(data);
}

void clean_tls(void*, const char* thread_id, int thread_id_len) {
    auto tlmap = shared_tlmap();
    tlmap->clean_thread_local(thread_id, thread_id_len);
}

} // namespace
}

extern "C" char* wilton_module_init() {
    try {
        auto err = wilton_register_tls_cleaner(nullptr, wilton::chakra::clean_tls);
        if (nullptr != err) wilton::support::throw_wilton_error(err, TRACEMSG(err));
        wilton::support::register_wiltoncall("runscript_chakra", wilton::chakra::runscript);
        return nullptr;
    } catch (const std::exception& e) {
        return wilton::support::alloc_copy(TRACEMSG(e.what() + "\nException raised"));
    }
}
